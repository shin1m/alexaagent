#ifndef ALEXAAGENT__JSON_H
#define ALEXAAGENT__JSON_H

#include <picojson/picojson.h>

template<typename T>
struct t_get
{
	std::string v_name;
};

template<typename T>
inline T& operator/(picojson::value& a_value, const t_get<T>& a_get)
{
	return a_value.get<picojson::value::object>().at(a_get.v_name).template get<T>();
}

template<typename T>
inline const T& operator/(const picojson::value& a_value, const t_get<T>& a_get)
{
	return a_value.get<picojson::value::object>().at(a_get.v_name).template get<T>();
}

inline t_get<bool> operator""_jsb(const char* a_name, size_t a_n)
{
	return t_get<bool>{std::string(a_name, a_n)};
}

inline t_get<double> operator""_jsn(const char* a_name, size_t a_n)
{
	return t_get<double>{std::string(a_name, a_n)};
}

inline t_get<std::string> operator""_jss(const char* a_name, size_t a_n)
{
	return t_get<std::string>{std::string(a_name, a_n)};
}

inline t_get<picojson::value::array> operator""_jsa(const char* a_name, size_t a_n)
{
	return t_get<picojson::value::array>{std::string(a_name, a_n)};
}

inline t_get<picojson::value::object> operator""_jso(const char* a_name, size_t a_n)
{
	return t_get<picojson::value::object>{std::string(a_name, a_n)};
}

inline picojson::value& operator/(picojson::value& a_value, const std::string& a_name)
{
	return a_value.get<picojson::value::object>().at(a_name);
}

inline const picojson::value& operator/(const picojson::value& a_value, const std::string& a_name)
{
	return a_value.get<picojson::value::object>().at(a_name);
}

inline std::pair<picojson::value::object::const_iterator, picojson::value::object::const_iterator> operator*(const picojson::value& a_value, const std::string& a_name)
{
	auto& x = a_value.get<picojson::value::object>();
	return std::make_pair(x.find(a_name), x.end());
}

template<typename T>
inline const T& operator|(const std::pair<picojson::value::object::const_iterator, picojson::value::object::const_iterator>& a_x, const T& a_y)
{
	return a_x.first == a_x.second ? a_y : a_x.first->second.get<T>();
}

inline bool operator!(const std::pair<picojson::value::object::const_iterator, picojson::value::object::const_iterator>& a_pair)
{
	return a_pair.first == a_pair.second;
}

inline const picojson::value& operator*(const std::pair<picojson::value::object::const_iterator, picojson::value::object::const_iterator>& a_pair)
{
	return a_pair.first->second;
}

struct t_set
{
	picojson::value& v_value;
	std::string v_name;
};

inline t_set operator<<(picojson::value& a_value, const std::string& a_name)
{
	return t_set{a_value, a_name};
}

template<typename T>
inline picojson::value& operator&(t_set a_set, T&& a_x)
{
	a_set.v_value.get<picojson::value::object>().emplace(a_set.v_name, picojson::value(std::forward<T>(a_x)));
	return a_set.v_value;
}

#endif
