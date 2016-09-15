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
	auto f_hex = [](int a_x)
	{
		return a_x + (a_x < 10 ? '0' : 'A' - 10);
	};
	for (; a_first != a_last; ++a_first) {
		unsigned char x = *a_first;
		if (f_uri_safe(x)) {
			*a_out++ = x;
		} else {
			*a_out++ = '%';
			*a_out++ = f_hex(x >> 4);
			*a_out++ = f_hex(x & 0xf);
		}
	}
	return a_out;
}

template<typename T_in, typename T_out>
T_out f_uri_decode(T_in a_first, T_in a_last, T_out a_out)
{
	auto f_hex = [](int a_x)
	{
		return std::isdigit(a_x) ? a_x - '0' : std::toupper(a_x) - 'A' + 10;
	};
	while (a_first != a_last) {
		unsigned char x = *a_first++;
		if (x == '%') {
			if (a_first == a_last) break;
			x = f_hex(*a_first++) << 4;
			if (a_first == a_last) break;
			x |= f_hex(*a_first++);
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

struct t_http
{
	boost::asio::streambuf v_buffer;
	std::string v_http;
	size_t v_code;
	std::string v_message;
	std::vector<std::string> v_headers;
	std::function<void(const std::string&)> v_read_until;

	template<typename T_socket>
	void f_send(T_socket& a_socket)
	{
		boost::asio::write(a_socket, v_buffer);
		boost::asio::read_until(a_socket, v_buffer, "\r\n");
		std::istream stream(&v_buffer);
		std::getline(stream >> v_http >> v_code, v_message);
		boost::asio::read_until(a_socket, v_buffer, "\r\n\r\n");
		std::string header;
		while (std::getline(stream, header) && header != "\r") v_headers.push_back(header);
	}
	void operator()(boost::asio::io_service& a_io, const std::string& a_url)
	{
		std::smatch match;
		if (!std::regex_match(a_url, match, std::regex{"(https?)://([^/]+)(.*)"})) throw std::runtime_error("invalid url");
		std::string path = match[3];
		std::ostream stream(&v_buffer);
		stream <<
		"GET " << (path.empty() ? "/" : path) << " HTTP/1.0\r\n"
		"Host: " << match[2] << "\r\n"
		"Accept: */*\r\n"
		"Connection: close\r\n\r\n";
		boost::asio::ip::tcp::resolver resolver(a_io);
		auto endpoint = resolver.resolve(boost::asio::ip::tcp::resolver::query(match[2], match[1]));
		if (match[1] == "http") {
			auto socket = std::make_shared<boost::asio::ip::tcp::socket>(a_io);
			boost::asio::connect(*socket, endpoint);
			f_send(*socket);
			v_read_until = [this, socket](auto& a_delimiter)
			{
				boost::asio::read_until(*socket, v_buffer, a_delimiter);
			};
		} else {
			auto tls = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12);
			tls->set_default_verify_paths();
			auto socket = std::make_shared<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(a_io, *tls);
			boost::asio::connect(socket->lowest_layer(), endpoint);
			socket->handshake(boost::asio::ssl::stream_base::client);
			f_send(*socket);
			v_read_until = [this, tls, socket](auto& a_delimiter)
			{
				boost::asio::read_until(*socket, v_buffer, a_delimiter);
			};
		}
	}
};

template<typename T_success, typename T_error>
void f_https_post(boost::asio::io_service& a_io, const std::string& a_host, const std::string& a_path, const std::map<std::string, std::string>& a_query, T_success a_success, T_error a_error)
{
	auto f_check = [a_error](const boost::system::error_code& a_ec)
	{
		if (a_ec) a_error(a_ec);
		return a_ec;
	};
	auto f_post = [a_host, a_path, a_success, f_check, query = f_build_query_string(a_query)](const std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>& a_socket)
	{
		auto request = std::make_shared<boost::asio::streambuf>();
		std::ostream(request.get()) <<
		"POST " << a_path << " HTTP/1.0\r\n"
		"Host: " << a_host << "\r\n"
		"Content-Length: " << query.size() << "\r\n"
		"Content-Type: application/x-www-form-urlencoded\r\n"
		"Cache-Control: no-cache\r\n\r\n" << query;
		boost::asio::async_write(*a_socket, *request, [a_success, f_check, a_socket, request](auto a_ec, auto)
		{
			if (f_check(a_ec)) return;
			auto response = std::make_shared<boost::asio::streambuf>();
			boost::asio::async_read_until(*a_socket, *response, "\r\n", [a_success, f_check, a_socket, response](auto a_ec, auto)
			{
				if (f_check(a_ec)) return;
				std::string http;
				size_t code;
				std::string message;
				std::getline(std::istream(response.get()) >> http >> code, message);
				boost::asio::async_read_until(*a_socket, *response, "\r\n\r\n", [a_success, f_check, a_socket, response, code](auto a_ec, auto)
				{
					if (f_check(a_ec)) return;
					std::vector<std::string> headers;
					std::istream s(response.get());
					std::string header;
					while (std::getline(s, header) && header != "\r") headers.push_back(header);
					boost::asio::async_read(*a_socket, *response, [a_success, a_socket, response, code, headers](auto a_ec, auto)
					{
						std::istream content(response.get());
						a_success(code, headers, content);
					});
				});
			});
		});
	};
	auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(a_io);
	resolver->async_resolve(boost::asio::ip::tcp::resolver::query(a_host, "https"), [&a_io, f_check, f_post, resolver](auto a_ec, auto a_i)
	{
		if (f_check(a_ec)) return;
		auto tls = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12);
		tls->set_default_verify_paths();
		auto socket = std::make_shared<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(a_io, *tls);
		boost::asio::async_connect(socket->lowest_layer(), a_i, [f_check, f_post, tls, socket](auto a_ec, auto)
		{
			if (!f_check(a_ec)) socket->async_handshake(boost::asio::ssl::stream_base::client, [f_check, f_post, socket](auto a_ec)
			{
				if (!f_check(a_ec)) f_post(socket);
			});
		});
	});
}

#endif
