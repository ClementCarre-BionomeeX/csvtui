#include "csv_buffer.h"

CSVBuffer::CSVBuffer(std::string filename) : filename(std::move(filename)) {}
size_t CSVBuffer::which_chunk(size_t line) { return line / chunk_size; }
