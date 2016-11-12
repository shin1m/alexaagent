#ifndef ALEXAAGENT__TINY_HTTP_H
#define ALEXAAGENT__TINY_HTTP_H

#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

inline bool f_uri_safe(char a_c)
{
	switch (a_c) {
	case '-':
	case '.':
	case '_':
	case '~':
		return true;
	default:
		return std::isalnum(a_c);
	}
}

template<typename T_in, typename T_out>
T_out f_uri_encode(T_in a_first, T_in a_last, T_out a_out)
{
	auto hex = [](int a_x)
	{
		return a_x + (a_x < 10 ? '0' : 'A' - 10);
	};
	for (; a_first != a_last; ++a_first) {
		unsigned char x = *a_first;
		if (f_uri_safe(x)) {
			*a_out++ = x;
		} else {
			*a_out++ = '%';
			*a_out++ = hex(x >> 4);
			*a_out++ = hex(x & 0xf);
		}
	}
	return a_out;
}

template<typename T_in, typename T_out>
T_out f_uri_decode(T_in a_first, T_in a_last, T_out a_out)
{
	auto hex = [](int a_x)
	{
		return std::isdigit(a_x) ? a_x - '0' : std::toupper(a_x) - 'A' + 10;
	};
	while (a_first != a_last) {
		unsigned char x = *a_first++;
		if (x == '%') {
			if (a_first == a_last) break;
			x = hex(*a_first++) << 4;
			if (a_first == a_last) break;
			x |= hex(*a_first++);
		}
		*a_out++ = x;
	}
	return a_out;
}

inline std::string f_uri_decode(const std::string& a_x)
{
	std::stringbuf s;
	f_uri_decode(a_x.begin(), a_x.end(), std::ostreambuf_iterator<char>(&s));
	return s.str();
}

inline std::string f_build_query_string(const std::map<std::string, std::string>& a_values)
{
	if (a_values.empty()) return std::string();
	std::stringbuf s;
	for (auto i = a_values.begin();;) {
		s.sputn(i->first.c_str(), i->first.size());
		s.sputc('=');
		f_uri_encode(i->second.begin(), i->second.end(), std::ostreambuf_iterator<char>(&s));
		if (++i == a_values.end()) break;
		s.sputc('&');
	}
	return s.str();
}

inline std::map<std::string, std::string> f_parse_query_string(const std::string& a_query)
{
	std::map<std::string, std::string> values;
	for (size_t i = 0; ; ++i) {
		size_t j = a_query.find('=', i);
		if (j == std::string::npos) break;
		auto key = a_query.substr(i, j - i);
		i = a_query.find('&', ++j);
		if (i == std::string::npos) {
			values.emplace(key, f_uri_decode(a_query.substr(j)));
			break;
		}
		values.emplace(key, f_uri_decode(a_query.substr(j, i - j)));
	}
	return values;
}

struct t_http10
{
	std::string v_service;
	std::string v_host;
	std::string v_path;
	boost::asio::streambuf v_buffer;
	std::string v_http;
	size_t v_code;
	std::string v_message;
	std::vector<std::string> v_headers;

	t_http10(const std::string& a_url)
	{
		std::smatch match;
		if (!std::regex_match(a_url, match, std::regex{"(https?)://([^/]+)(.*)"})) throw std::runtime_error("invalid url");
		v_service = match[1];
		v_host = match[2];
		v_path = match[3];
	}
	t_http10& operator()(const char* a_method)
	{
		std::ostream(&v_buffer) << a_method << ' ' << (v_path.empty() ? "/" : v_path) << " HTTP/1.0\r\n"
		"Host: " << v_host << "\r\n\r\n";
		return *this;
	}
	t_http10& operator()(const char* a_method, const std::string& a_data, const char* a_content_type = "application/octet-stream")
	{
		std::ostream(&v_buffer) << a_method << ' ' << (v_path.empty() ? "/" : v_path) << " HTTP/1.0\r\n"
		"Host: " << v_host << "\r\n"
		"Content-Length: " << a_data.size() << "\r\n"
		"Content-Type: " << a_content_type << "\r\n"
		"Cache-Control: no-cache\r\n\r\n" << a_data;
		return *this;
	}
	t_http10& operator()(const char* a_method, const std::map<std::string, std::string>& a_query)
	{
		return (*this)(a_method, f_build_query_string(a_query), "application/x-www-form-urlencoded");
	}
	template<typename T_receive>
	void operator()(boost::asio::io_service& a_io, T_receive a_receive)
	{
		auto send = [&](auto& a_socket)
		{
			boost::asio::write(a_socket, v_buffer);
			boost::asio::read_until(a_socket, v_buffer, "\r\n");
			std::istream stream(&v_buffer);
			std::getline(stream >> v_http >> v_code, v_message);
			boost::asio::read_until(a_socket, v_buffer, "\r\n\r\n");
			std::string header;
			while (std::getline(stream, header) && header != "\r") v_headers.push_back(header);
			a_receive(a_socket);
		};
		boost::asio::ip::tcp::resolver resolver(a_io);
		auto endpoint = resolver.resolve({v_host, v_service});
		if (v_service == "http") {
			auto socket = std::make_shared<boost::asio::ip::tcp::socket>(a_io);
			boost::asio::connect(*socket, endpoint);
			send(*socket);
		} else {
			auto tls = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12);
			tls->set_default_verify_paths();
			auto socket = std::make_shared<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(a_io, *tls);
			boost::asio::connect(socket->lowest_layer(), endpoint);
			socket->handshake(boost::asio::ssl::stream_base::client);
			send(*socket);
		}
	}
	template<typename T_success, typename T_error>
	void operator()(boost::asio::io_service& a_io, T_success a_success, T_error a_error)
	{
		auto check = [a_error](auto a_ec) mutable
		{
			if (a_ec) a_error(a_ec);
			return a_ec;
		};
		auto send = [this, a_success, check](auto& a_socket) mutable
		{
			boost::asio::async_write(*a_socket, v_buffer, [this, a_success, check, a_socket](auto a_ec, auto) mutable
			{
				if (!check(a_ec)) boost::asio::async_read_until(*a_socket, v_buffer, "\r\n", [this, a_success, check, a_socket](auto a_ec, auto) mutable
				{
					if (check(a_ec)) return;
					std::getline(std::istream(&v_buffer) >> v_http >> v_code, v_message);
					boost::asio::async_read_until(*a_socket, v_buffer, "\r\n\r\n", [this, a_success, check, a_socket](auto a_ec, auto) mutable
					{
						if (check(a_ec)) return;
						std::istream stream(&v_buffer);
						std::string header;
						while (std::getline(stream, header) && header != "\r") v_headers.push_back(header);
						a_success(a_socket);
					});
				});
			});
		};
		auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(a_io);
		resolver->async_resolve({v_host, v_service}, [this, &a_io, check, send, resolver](auto a_ec, auto a_i) mutable
		{
			if (check(a_ec)) return;
			if (v_service == "http") {
				auto socket = std::make_shared<boost::asio::ip::tcp::socket>(a_io);
				boost::asio::async_connect(*socket, a_i, [check, send, socket](auto a_ec, auto) mutable
				{
					if (!check(a_ec)) send(socket);
				});
			} else {
				auto tls = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12);
				tls->set_default_verify_paths();
				auto socket = std::make_shared<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(a_io, *tls);
				boost::asio::async_connect(socket->lowest_layer(), a_i, [check, send, tls, socket](auto a_ec, auto) mutable
				{
					if (!check(a_ec)) socket->async_handshake(boost::asio::ssl::stream_base::client, [check, send, socket](auto a_ec) mutable
					{
						if (!check(a_ec)) send(socket);
					});
				});
			}
		});
	}
};

#endif
