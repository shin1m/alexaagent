#include <cassert>

#include "multipart.h"

struct t_target
{
	std::vector<std::string> v_log;

	void f_boundary()
	{
		v_log.push_back("boundary");
	}
	void f_part(const std::string& a_type, const std::string& a_id)
	{
		v_log.push_back("part: " + a_type + ", " + a_id);
	}
	void f_content(const char* a_p, size_t a_n)
	{
		v_log.push_back("content: " + std::string(a_p, a_n));
	}
};

int main(int argc, char* argv[])
{
	t_target target;
	t_multipart<t_target> multipart(target, "foo");
	for (char c :
"--foo\r\n"
"Content-Type: application/json; charset=UTF-8\r\n"
"\r\n"
"{\r\n"
"}\r\n"
"--foo\r\n"
"Content-Type: application/octet-stream\r\n"
"Content-ID: <bar>\r\n"
"\r\n"
"\r\n"
"--foo\r\n"
"Content-Type: application/octet-stream\r\n"
"Content-ID: <zot>\r\n"
"\r\n"
"--fo\r\n"
"--fooo\r\n"
"--foo-\r\n"
"--foo---\r\n"
"--foo--\r\n"
	) multipart(c);
	for (auto& x : target.v_log) std::fprintf(stderr, "%s\n", x.c_str());
	size_t i = 0;
	assert(target.v_log[i++] == "boundary");
	assert(target.v_log[i++] == "part: application/json, ");
	assert(target.v_log[i++] == "content: {");
	assert(target.v_log[i++] == "content: \r\n");
	assert(target.v_log[i++] == "content: }");
	assert(target.v_log[i++] == "boundary");
	assert(target.v_log[i++] == "part: application/octet-stream, bar");
	assert(target.v_log[i++] == "content: ");
	assert(target.v_log[i++] == "boundary");
	assert(target.v_log[i++] == "part: application/octet-stream, zot");
	assert(target.v_log[i++] == "content: --fo");
	assert(target.v_log[i++] == "content: ");
	assert(target.v_log[i++] == "content: \r\n--foo");
	assert(target.v_log[i++] == "content: o");
	assert(target.v_log[i++] == "content: \r\n--foo");
	assert(target.v_log[i++] == "content: -");
	assert(target.v_log[i++] == "content: ");
	assert(target.v_log[i++] == "content: \r\n--foo");
	assert(target.v_log[i++] == "content: --");
	assert(target.v_log[i++] == "content: -");
	assert(target.v_log[i++] == "boundary");
	return 0;
}
