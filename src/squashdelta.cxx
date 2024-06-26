/**
 * SquashFS delta tools
 * (c) 2014 Michał Górny
 * Released under the terms of the 2-clause BSD license
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <iostream>
#include <list>
#include <typeinfo>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C"
{
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>
}

#include "compressor.hxx"
#include "hash.hxx"
#include "squashfs.hxx"
#include "util.hxx"

/**
 * In-memory representation of a compressed block.
 */
struct compressed_block
{
	size_t offset;
	size_t length;
	size_t uncompressed_length;
	uint32_t hash;
};

#pragma pack(push, 1)
/**
 * On-disk representation of a compressed block.
 *
 * Integers are stored in network byte order (big-endian).
 */
struct serialized_compressed_block
{
	uint64_t offset;
	uint32_t length;
	uint32_t uncompressed_length;
};

/**
 * On-disk representation of a squashdelta file header.
 */
struct sqdelta_header
{
	uint32_t magic;
	uint32_t flags;
	uint32_t compression;
	uint32_t block_count;
};
#pragma pack(pop)

/**
 * @brief Convert a 64-bit integer from host to network byte order.
 *
 * This function converts a 64-bit integer from host byte order to network byte order.
 * It performs a byte swap if the system is little-endian.
 *
 * @param hostlonglong The 64-bit integer in host byte order.
 * @return The 64-bit integer in network byte order.
 */
#ifndef htonll
uint64_t htonll(uint64_t hostlonglong)
{
	// Check system endianness
	static const int one = 1;
	if (*(const char *)&one == 1)
	{ // Little endian
		// Perform byte swap
		return ((uint64_t)htonl((uint32_t)(hostlonglong & 0xFFFFFFFF)) << 32) | htonl((uint32_t)(hostlonglong >> 32));
	}
	else
	{
		// Big endian - no need to swap bytes
		return hostlonglong;
	}
}
#endif

const uint32_t sqdelta_magic = 0x5371ceb4;

bool sort_by_offset(const struct compressed_block &lhs,
					const struct compressed_block &rhs)
{
	return lhs.offset < rhs.offset;
}

bool sort_by_len_hash(const struct compressed_block &lhs,
					  const struct compressed_block &rhs)
{
	if (lhs.length == rhs.length)
		return lhs.hash < rhs.hash;
	return lhs.length < rhs.length;
}

/**
 * @brief This function retrieves the list of compressed blocks from a SquashFS filesystem.
 *
 * The function takes a reference to an MMAPFile object and a reference to a pointer to a Compressor object.
 * The MMAPFile object represents the SquashFS filesystem from which the blocks are to be retrieved.
 * The Compressor object is used to decompress the blocks.
 *
 * The function reads the superblock of the SquashFS filesystem, checks its validity, and retrieves the block size.
 * It then reads the inodes and fragments, and records the compressed blocks.
 * The function also sorts the blocks by offset to optimize for sequential reads.
 *
 * @param f A reference to an MMAPFile object representing the SquashFS filesystem.
 * @param c A reference to a pointer to a Compressor object used for decompression.
 * @param block_size A reference to a size_t variable where the block size is stored.
 * @return A list of compressed_block structs representing the compressed blocks in the SquashFS filesystem.
 */
std::list<struct compressed_block> get_blocks(MMAPFile &f, Compressor *&c, size_t &block_size)
{
	// read & verify superblock
	const squashfs::super_block &sb = f.read<squashfs::super_block>();

	if (sb.s_magic != squashfs::magic)
		throw std::runtime_error(
			"File is not a valid SquashFS image (no magic).");
	if (sb.s_major != 4 || sb.s_minor != 0)
		throw std::runtime_error("File is not SquashFS 4.0");

	if (!block_size)
		block_size = sb.block_size;
	else if (block_size != sb.block_size)
		throw std::runtime_error("Input files have different block sizes");

	switch (sb.compression)
	{
	case squashfs::compression::lzo:
#ifdef ENABLE_LZO
		if (!c)
			c = new LZOCompressor();
		else if (typeid(*c) != typeid(LZOCompressor))
			throw std::runtime_error("The two files use different compressors");
#else
		throw std::runtime_error("LZO compression support disabled at build time");
#endif
		break;
	case squashfs::compression::lz4:
#ifdef ENABLE_LZ4
		if (!c)
			c = new LZ4Compressor();
		else if (typeid(*c) != typeid(LZ4Compressor))
			throw std::runtime_error("The two files use different compressors");
#else
		throw std::runtime_error("LZ4 compression support disabled at build time");
#endif
		break;
	default:
		throw std::runtime_error("Unsupported compression algorithm.");
	}

	MetadataReader coptsr(f, sizeof(sb), *c);
	c->setup(sb.flags & squashfs::flags::compression_options
				 ? &coptsr
				 : 0);
	coptsr.block_num();

	// read inodes
	std::list<struct compressed_block>
		compressed_metadata_blocks,
		compressed_data_blocks;

	std::cerr << "Reading inodes..." << std::endl;

	InodeReader ir(f, sb, *c);

	for (uint32_t i = 0; i < sb.inodes; ++i)
	{
		union squashfs::inode::inode &in = ir.read();

		if (in.as_base.inode_type == squashfs::inode::type::reg || in.as_base.inode_type == squashfs::inode::type::lreg)
		{
			uint64_t pos;
			uint64_t block_count;
			le32 *block_list;

			if (in.as_base.inode_type == squashfs::inode::type::reg)
			{
				pos = in.as_reg.start_block;
				block_count = in.as_reg.block_count(sb.block_size, sb.block_log);
				block_list = in.as_reg.block_list();
			}
			else
			{
				pos = in.as_lreg.start_block;
				block_count = in.as_lreg.block_count(sb.block_size, sb.block_log);
				block_list = in.as_lreg.block_list();
			}

			for (uint64_t j = 0; j < block_count; ++j)
			{
				if (block_list[j] & squashfs::block_size::uncompressed)
				{
					// seek over the uncompressed block
					uint32_t len = (block_list[j] & ~squashfs::block_size::uncompressed);
					assert(len != 0);
					pos += len;
				}
				// if length == 0, it indicates a sparse block
				else if (block_list[j] != 0)
				{
					// record the compressed block
					struct compressed_block block;
					block.offset = pos;
					block.length = block_list[j];

					compressed_data_blocks.push_back(block);
					pos += block.length;
				}
			}
		}
	}

	size_t block_num = ir.block_num();
	std::cerr << "Read " << sb.inodes << " inodes in "
			  << block_num << " blocks.\n";

	// record inode blocks

	std::cerr << "Hashing " << block_num
			  << " inode blocks..." << std::endl;

	MetadataBlockReader mir(f, sb.inode_table_start, *c);
	for (size_t i = 0; i < block_num; ++i)
	{
		const void *data;
		size_t pos;
		size_t length;
		bool compressed;

		mir.read_input_block(&data, &pos, &length, &compressed);
		assert(length != 0);

		if (compressed)
		{
			struct compressed_block block;
			block.offset = pos;
			block.length = length;
			block.hash = murmurhash3(data, length, 0);

			compressed_metadata_blocks.push_back(block);
		}
	}

	// fragments
	std::cerr << "Reading fragment table..." << std::endl;

	FragmentTableReader fr(f, sb, *c);

	for (uint32_t i = 0; i < sb.fragments; ++i)
	{
		const struct squashfs::fragment_entry &fe = fr.read();
		assert(fe.size != 0);

		if (!(fe.size & squashfs::block_size::uncompressed))
		{
			struct compressed_block block;
			block.offset = fe.start_block;
			block.length = fe.size;

			compressed_data_blocks.push_back(block);
		}
	}

	block_num = fr.block_num();
	std::cerr << "Read " << sb.fragments << " fragments in "
			  << block_num << " blocks.\n";

	// record fragment table

	std::cerr << "Hashing " << block_num
			  << " fragment table blocks..." << std::endl;

	MetadataBlockReader mfr(f, fr.start_offset, *c);
	for (size_t i = 0; i < block_num; ++i)
	{
		const void *data;
		size_t pos;
		size_t length;
		bool compressed;

		mir.read_input_block(&data, &pos, &length, &compressed);

		if (compressed)
		{
			struct compressed_block block;
			block.offset = pos;
			block.length = length;
			block.hash = murmurhash3(data, length, 0);

			compressed_metadata_blocks.push_back(block);
		}
	}

	// sort by offset to use sequential reads
	compressed_data_blocks.sort(sort_by_offset);

	std::cerr << "Hashing " << compressed_data_blocks.size()
			  << " data blocks..." << std::endl;
	MMAPFile hf(f);

	// record the checksums and perform initial deduplication
	for (std::list<struct compressed_block>::iterator
			 i = compressed_data_blocks.begin(),
			 j = compressed_data_blocks.end();
		 i != compressed_data_blocks.end();)
	{
		// duplicates will be adjacent after sorting
		if ((*i).offset == ((*j).offset))
		{
			assert((*i).length == (*j).length);
			i = compressed_data_blocks.erase(i);
			continue;
		}

		hf.seek((*i).offset, std::ios::beg);
		(*i).hash = murmurhash3(hf.read_array<uint8_t>((*i).length),
								(*i).length, 0);
		j = i++;
	}

	compressed_data_blocks.splice(compressed_data_blocks.end(),
								  compressed_metadata_blocks);

	std::cerr << "Total: " << compressed_data_blocks.size()
			  << " compressed blocks." << std::endl;

	return compressed_data_blocks;
}

void write_unpacked_file(SparseFileWriter &outf, MMAPFile &inf,
						 std::list<struct compressed_block> &cb, Compressor &c,
						 size_t block_size)
{
	size_t prev_offset = 0;
	inf.seek(0, std::ios::beg);

	for (std::list<struct compressed_block>::iterator i = cb.begin();
		 i != cb.end(); ++i)
	{
		assert((*i).offset >= prev_offset);

		size_t pre_length = (*i).offset - prev_offset;
		prev_offset = (*i).offset + (*i).length;

		// first, copy the data preceeding compressed block
		outf.write(inf.read_array<char>(pre_length), pre_length);

		// then, seek through the block
		inf.seek((*i).length);
		outf.write_sparse((*i).length);
	}

	// write the last block
	outf.write(inf.read_array<char>(inf.getlen() - prev_offset),
			   inf.getlen() - prev_offset);

	char *buf = new char[block_size];
	try
	{
		for (std::list<struct compressed_block>::iterator i = cb.begin();
			 i != cb.end(); ++i)
		{
			size_t unc_length;

			inf.seek((*i).offset, std::ios::beg);
			unc_length = c.decompress(buf, inf.read_array<char>((*i).length),
									  (*i).length, block_size);

			(*i).uncompressed_length = unc_length;
			outf.write(buf, unc_length);
		}
	}
	catch (std::exception &e)
	{
		delete[] buf;
		throw;
	}
	delete[] buf;
}

void write_block_list(SparseFileWriter &outf, sqdelta_header h,
					  std::list<struct compressed_block> &cb, bool at_end = true)
{
	// store the block count in header
	h.block_count = htonl(cb.size());

	if (!at_end)
		outf.write<struct sqdelta_header>(h);

	for (std::list<struct compressed_block>::iterator i = cb.begin();
		 i != cb.end(); ++i)
	{
		struct serialized_compressed_block b;

		b.offset = htonll((*i).offset);
		b.length = htonl((*i).length);
		b.uncompressed_length = htonl((*i).uncompressed_length);

		outf.write<struct serialized_compressed_block>(b);
	}

	if (at_end)
		outf.write<struct sqdelta_header>(h);
}

int main(int argc, char *argv[])
{
	if (argc < 4)
	{
		std::cerr << "Usage: " << argv[0] << " <source> <target> <patch-output>\n";
		return 1;
	}

	const char *source_file = argv[1];
	const char *target_file = argv[2];
	const char *patch_file = argv[3];

	try
	{
		MMAPFile source_f, target_f;

		std::list<struct compressed_block> source_blocks;
		std::list<struct compressed_block> target_blocks;

		Compressor *c = 0;
		size_t block_size = 0;

		try
		{
			source_f.open(source_file);
			std::cerr << "Source: " << source_file << "\n";
			source_blocks = get_blocks(source_f, c, block_size);
		}
		catch (IOError &e)
		{
			std::cerr << "Program terminated abnormally:\n\t"
					  << e.what() << "\n\tat file: " << source_file
					  << "\n\terrno: " << strerror(e.errno_val) << "\n";
			if (c)
				delete c;
			return 1;
		}
		catch (std::exception &e)
		{
			std::cerr << "Program terminated abnormally:\n\t"
					  << e.what() << "\n\tat file: " << source_file << "\n";
			if (c)
				delete c;
			return 1;
		}

		std::cerr << "\n";

		try
		{
			target_f.open(target_file);
			std::cerr << "Target: " << target_file << "\n";
			target_blocks = get_blocks(target_f, c, block_size);
		}
		catch (IOError &e)
		{
			std::cerr << "Program terminated abnormally:\n\t"
					  << e.what() << "\n\tat file: " << source_file
					  << "\n\terrno: " << strerror(e.errno_val) << "\n";
			delete c;
			return 1;
		}
		catch (std::exception &e)
		{
			std::cerr << "Program terminated abnormally:\n\t"
					  << e.what() << "\n\tat file: " << source_file << "\n";
			delete c;
			return 1;
		}

		std::cerr << "\n";

		source_blocks.sort(sort_by_len_hash);
		target_blocks.sort(sort_by_len_hash);

		for (std::list<struct compressed_block>::iterator
				 i = source_blocks.begin(),
				 j = target_blocks.begin();
			 i != source_blocks.end() && j != target_blocks.end();)
		{
			// seek until we find duplicates
			if ((*i).length < (*j).length)
				++i;
			else if ((*j).length < (*i).length)
				++j;
			else if ((*i).hash < (*j).hash)
				++i;
			else if ((*j).hash < (*i).hash)
				++j;
			else
			{
				// found a match, remove the blocks then
				std::list<struct compressed_block>::iterator
					i_st = i,
					j_st = j;

				// remove consecutive duplicates as well
				while (i != source_blocks.end() && (*i).length == (*i_st).length && (*i).hash == (*i_st).hash)
					++i;
				while (j != target_blocks.end() && (*j).length == (*j_st).length && (*j).hash == (*j_st).hash)
					++j;

				source_blocks.erase(i_st, i);
				target_blocks.erase(j_st, j);
			}
		}

		std::cerr << "Unique blocks found: "
				  << source_blocks.size() << " in source and "
				  << target_blocks.size() << " in target.\n";

		// now we need to write the expanded files

		source_blocks.sort(sort_by_offset);
		target_blocks.sort(sort_by_offset);

		// open output before changing cwd
		SparseFileWriter patch_out;
		patch_out.open(patch_file);

		const char *tmpdir = getenv("TMPDIR");
#ifdef _P_tmpdir
		if (!tmpdir)
			tmpdir = P_tmpdir;
#endif
		if (!tmpdir)
			tmpdir = "/tmp";

		if (chdir(tmpdir) == -1)
		{
			std::cerr << "Unable to chdir() into temporary directory\n"
						 "\tDirectory: "
					  << tmpdir << "\n";
			delete c;
			return 1;
		}

		struct sqdelta_header dh;
		dh.flags = htonl(0);
		dh.magic = htonl(sqdelta_magic);
		dh.compression = htonl(c->get_compression_value());

		TemporarySparseFileWriter source_temp, target_temp;
		try
		{
			std::cerr << "Writing expanded source file..." << std::endl;

			c->reset();
			source_temp.open(source_f.getlen());
			write_unpacked_file(source_temp, source_f, source_blocks, *c,
								block_size);
			write_block_list(source_temp, dh, source_blocks);
		}
		catch (IOError &e)
		{
			std::cerr << "Program terminated abnormally:\n\t"
					  << e.what() << "\n\tat temporary file for source"
					  << "\n\terrno: " << strerror(e.errno_val) << "\n";
			delete c;
			return 1;
		}
		catch (std::exception &e)
		{
			std::cerr << "Program terminated abnormally:\n\t"
					  << e.what() << "\n\tat temporary file for source\n";
			delete c;
			return 1;
		}

		try
		{
			std::cerr << "Writing expanded target file..." << std::endl;

			c->reset();
			target_temp.open(target_f.getlen());
			write_unpacked_file(target_temp, target_f, target_blocks, *c,
								block_size);
			write_block_list(target_temp, dh, target_blocks);
		}
		catch (IOError &e)
		{
			std::cerr << "Program terminated abnormally:\n\t"
					  << e.what() << "\n\tat temporary file for target"
					  << "\n\terrno: " << strerror(e.errno_val) << "\n";
			delete c;
			return 1;
		}
		catch (std::exception &e)
		{
			std::cerr << "Program terminated abnormally:\n\t"
					  << e.what() << "\n\tat temporary file for target\n";
			delete c;
			return 1;
		}

		delete c;

		write_block_list(patch_out, dh, source_blocks, false);

		std::cerr << "Calling xdelta to generate the diff..." << std::endl;

		pid_t child = fork();
		if (child == -1)
			throw IOError("fork() failed", errno);
		if (child == 0)
		{
			try
			{
				// in child
				if (close(1) == -1)
					throw IOError("Unable to close stdout", errno);
				if (dup2(patch_out.fd, 1) == -1)
					throw IOError("Unable to override stdout via dup2()", errno);

				if (execlp("xdelta3",
						   "xdelta3", "-v", "-9", "-S", "djw",
						   "-s", source_temp.name(), target_temp.name(),
						   static_cast<const char *>(0)) == -1)
					throw IOError("execlp() failed", errno);
			}
			catch (IOError &e)
			{
				std::cerr << "Error occured in child process:\n\t"
						  << e.what() << "\n\terrno: " << strerror(e.errno_val) << "\n";
				return 1;
			}
		}
		else
		{
			int status;

			waitpid(child, &status, 0);

			if (WEXITSTATUS(status) != 0)
			{
				std::cerr << "Child process terminate with error status\n"
							 "\treturn code: "
						  << WEXITSTATUS(status) << "\n";
				return 1;
			}
		}

		target_temp.close();
		source_temp.close();
		patch_out.close();
	}
	catch (IOError &e)
	{
		std::cerr << "Error occured:\n\t"
				  << e.what() << "\n\terrno: " << strerror(e.errno_val) << "\n";
		return 1;
	}

	return 0;
}
