#include <cassert>

#include "tiny_http.h"

int main(int argc, char* argv[])
{
	{
		std::string source = "-._~AZ09\x0f\x10\xff";
		std::stringbuf s;
		f_uri_encode(source.begin(), source.end(), std::ostreambuf_iterator<char>(&s));
		assert("-._~AZ09%0F%10%FF" == s.str());
	}
	{
		std::string source = "-._~AZ09%0F%10%ff";
		std::stringbuf s;
		f_uri_decode(source.begin(), source.end(), std::ostreambuf_iterator<char>(&s));
		assert("-._~AZ09\x0f\x10\xff" == s.str());
	}
	assert("-._~AZ09\x0f\x10\xff" == f_uri_decode("-._~AZ09%0F%10%ff"));
	{
		auto query = f_build_query_string({
			{"b", ""},
			{"c", "bar"},
			{"a", "foo"}
		});
		assert("a=foo&b=&c=bar" == query);
	}
	{
		auto query = f_parse_query_string("a=foo&b=&c=b%61r");
		assert(3 == query.size());
		assert("foo" == query["a"]);
		assert("" == query["b"]);
		assert("bar" == query["c"]);
	}
	return 0;
}
