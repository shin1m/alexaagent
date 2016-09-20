#include "tiny_http.h"

int main(int argc, char* argv[])
{
	if (argc != 2) return -1;
	boost::asio::io_service io;
	t_http10 http(argv[1]);
	http("GET")(io, [&](auto& a_socket)
	{
		std::fprintf(stderr, "%s %d%s\n", http.v_http.c_str(), http.v_code, http.v_message.c_str());
		for (auto& header : http.v_headers) std::fprintf(stderr, "%s\n", header.c_str());
		boost::asio::read_until(a_socket, http.v_buffer, "\n");
		std::string line;
		std::getline(std::istream(&http.v_buffer), line);
		std::fprintf(stderr, "\n%s\n", line.c_str());
	});
	return 0;
}
