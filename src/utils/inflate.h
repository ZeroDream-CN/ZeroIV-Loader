#pragma once

#include <string>

// Raw deflate decompression (RFC 1951), no zlib wrapper: that is the form an
// RPF7 entry stores a compressed payload in.
//
// Written here rather than pulled in as a dependency because the loader needs
// one function and speed is irrelevant: the only payload it ever inflates is
// dlclist.xml, a few kilobytes.
namespace inflate
{
	// Decompresses `input` into `out`. Returns false and fills `error` on
	// malformed input, so a corrupt archive is reported instead of half-decoded.
	bool raw(std::string& out, const std::string& input, std::string& error);
}
