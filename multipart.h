#ifndef ALEXAAGENT__MULTIPART_H
#define ALEXAAGENT__MULTIPART_H

#include <regex>
#include <vector>

template<typename T_target>
class t_multipart
{
	static std::regex v_re_content_type;
	static std::regex v_re_content_id;

	T_target& v_target;
	std::string v_boundary;
	const char* v_suffix;
	size_t v_last = 2;;
	char v_buffer[1024];
	char* v_p = v_buffer;
	size_t (t_multipart::*v_write)(const char*, size_t) = &t_multipart::f_write_ignore;
	std::vector<char> v_line;
	std::string v_content_type;
	std::string v_content_id;

	size_t f_write_ignore(const char* a_p, size_t a_n)
	{
		return a_n;
	}
	size_t f_write_header(const char* a_p, size_t a_n)
	{
		for (size_t i = 0; i < a_n; ++i) {
			char c = a_p[i];
			if (c == '\n' && !v_line.empty() && v_line.back() == '\r') {
				if (v_line.size() == 1) {
					v_target.f_part(v_content_type, v_content_id);
					v_content_type.clear();
					v_content_id.clear();
					v_line.clear();
					v_write = &t_multipart::f_write_content;
					return ++i;
				} else {
					std::match_results<std::vector<char>::iterator> match;
					if (std::regex_match(v_line.begin(), v_line.end(), match, v_re_content_type))
						v_content_type = match[1].str();
					else if (std::regex_match(v_line.begin(), v_line.end(), match, v_re_content_id))
						v_content_id = match[1].str();
					v_line.clear();
				}
			} else {
				v_line.push_back(c);
			}
		}
		return a_n;
	}
	size_t f_write_content(const char* a_p, size_t a_n)
	{
		v_target.f_content(a_p, a_n);
		return a_n;
	}
	void f_write(const char* a_p, size_t a_n)
	{
		while (true) {
			size_t n = (this->*v_write)(a_p, a_n);
			a_n -= n;
			if (a_n <= 0) break;
			a_p += n;
		}
	}
	void f_flush()
	{
		f_write(v_buffer, v_p - v_buffer);
		v_p = v_buffer;
	}

public:
	t_multipart(T_target& a_target, const std::string& a_boundary) : v_target(a_target), v_boundary("\r\n--" + a_boundary)
	{
	}
	void operator()(char a_c)
	{
		if (v_last > 0) {
			if (v_last < v_boundary.size()) {
				if (a_c == v_boundary[v_last]) {
					++v_last;
					return;
				}
				f_write(v_boundary.c_str(), v_last);
			} else {
				size_t i = v_last - v_boundary.size();
				if (i == 0) {
					switch (a_c) {
					case '\r':
						v_suffix = "\r\n";
						++v_last;
						return;
					case '-':
						v_suffix = "--\r\n";
						++v_last;
						return;
					}
					f_write(v_boundary.c_str(), v_last);
				} else if (a_c == v_suffix[i]) {
					if (v_suffix[++i] == '\0') {
						v_target.f_boundary();
						v_write = i == 2 ? &t_multipart::f_write_header : &t_multipart::f_write_ignore;
						v_last = 0;
					} else {
						++v_last;
					}
					return;
				} else {
					f_write(v_boundary.c_str(), v_boundary.size());
					f_write(v_suffix, i);
				}
			}
			v_last = 0;
		}
		if (a_c == '\r') {
			f_flush();
			v_last = 1;
		} else {
			*v_p = a_c;
			if (++v_p >= v_buffer + sizeof(v_buffer)) f_flush();
		}
	}
};

template<typename T_target>
std::regex t_multipart<T_target>::v_re_content_type("content-type:\\s*([\\-0-9a-z]+/[\\-0-9a-z]+).*\r", std::regex::ECMAScript | std::regex::icase);
template<typename T_target>
std::regex t_multipart<T_target>::v_re_content_id("content-id:\\s*<\\s*(\\S+)\\s*>.*\r", std::regex::ECMAScript | std::regex::icase);

#endif
