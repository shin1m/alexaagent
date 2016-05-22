#include <nghttp2/asio_http2_server.h>

#include "session.h"
#include "tiny_http.h"

int main(int argc, char* argv[])
{
	picojson::value configuration;
	{
		std::ifstream s("alexaagent.json");
		s >> configuration;
	}
	auto profile = configuration / "profile";
	auto sounds = configuration / "sounds";
	av_register_all();
	avformat_network_init();
	std::unique_ptr<ALCdevice, decltype(&alcCloseDevice)> device(alcOpenDevice(NULL), alcCloseDevice);
	if (!device) throw std::runtime_error("alcOpenDevice");
	std::unique_ptr<ALCcontext, void (*)(ALCcontext*)> context(alcCreateContext(device.get(), NULL), [](auto a_x)
	{
		alcMakeContextCurrent(NULL);
		alcDestroyContext(a_x);
	});
	alcMakeContextCurrent(context.get());
	alGetError();
	nghttp2::asio_http2::server::http2 server;
	std::unique_ptr<t_scheduler> scheduler;
	std::unique_ptr<t_session> session;
	std::function<void(const std::string&)> f_refresh;
	auto grant = [&](std::map<std::string, std::string>&& a_query, std::function<void()> a_done)
	{
		a_query.emplace("client_id", profile / "client_id"_jss);
		a_query.emplace("client_secret", profile / "client_secret"_jss);
		f_https_post(*server.io_services().front(), "api.amazon.com", "/auth/o2/token", a_query, [&, a_done](auto a_code, auto&, auto& a_content)
		{
			picojson::value result;
			a_content >> result;
#ifdef ALEXAAGENT_LOG
			std::fprintf(stderr, "grant: %d, %s\n", a_code, result.serialize(true).c_str());
#endif
			if (a_code == 200) {
				auto access_token = result / "access_token"_jss;
				auto refresh_token = result / "refresh_token"_jss;
				std::ofstream("session/token") << refresh_token;
				if (session)
					session->f_token(access_token);
				else
					session.reset(new t_session(*scheduler, access_token, sounds));
				scheduler->f_run_in(std::chrono::seconds(static_cast<size_t>(result / "expires_in"_jsn)), [&, refresh_token](auto)
				{
					f_refresh(refresh_token);
				});
			}
			a_done();
		}, [a_done](auto a_ec)
		{
			std::fprintf(stderr, "grant: %s\n", a_ec.message().c_str());
			a_done();
		});
	};
	f_refresh = [&](const std::string& a_token)
	{
		grant({
			{"grant_type", "refresh_token"},
			{"refresh_token", a_token}
		}, [] {});
	};
	server.handle("/", [&](auto& a_request, auto& a_response)
	{
		if (std::ifstream("session/token")) {
			a_response.write_head(200);
			a_response.end(nghttp2::asio_http2::file_generator("index.html"));
		} else {
			picojson::value data(picojson::value::object{
				{"alexa:all", picojson::value(picojson::value::object{
					{"productID", profile / "product_id"},
					{"productInstanceAttributes", picojson::value(picojson::value::object{
						{"deviceSerialNumber", profile / "device_serial_number"}
					})}
				})},
			});
			nghttp2::asio_http2::server::redirect_handler(302, "https://www.amazon.com/ap/oa?" + f_build_query_string({
				{"client_id", profile / "client_id"_jss},
				{"scope", "alexa:all"},
				{"scope_data", data.serialize()},
				{"response_type", "code"},
				{"redirect_uri", profile / "redirect_uri"_jss}
			}))(a_request, a_response);
		}
	});
	server.handle("/grant", [&](auto& a_request, auto& a_response)
	{
		auto f = std::bind(nghttp2::asio_http2::server::redirect_handler(302, "/"), std::ref(a_request), std::ref(a_response));
		auto values = f_parse_query_string(a_request.uri().raw_query);
		auto i = values.find("code");
		if (i == values.end())
			f();
		else
			grant({
				{"grant_type", "authorization_code"},
				{"code", i->second},
				{"redirect_uri", profile / "redirect_uri"_jss}
			}, f);
	});
	server.handle("/recognize/start", [&](auto& a_request, auto& a_response)
	{
		if (session) session->f_capture_force(true);
		a_response.write_head(session ? 200 : 500);
		a_response.end();
	});
	server.handle("/recognize/finish", [&](auto& a_request, auto& a_response)
	{
		if (session) session->f_capture_force(false);
		a_response.write_head(session ? 200 : 500);
		a_response.end();
	});
	boost::system::error_code ec;
	boost::asio::ssl::context tls(boost::asio::ssl::context::tlsv12);
	tls.use_private_key_file("server.key", boost::asio::ssl::context::pem);
	tls.use_certificate_chain_file("server.crt");
	nghttp2::asio_http2::server::configure_tls_context_easy(ec, tls);
	if (server.listen_and_serve(ec, tls, "localhost", "3000", true)) throw std::runtime_error(ec.message());
	scheduler.reset(new t_scheduler(*server.io_services().front()));
	boost::asio::signal_set signals(*server.io_services().front(), SIGINT);
	signals.async_wait([&](auto, auto a_signal)
	{
		std::fprintf(stderr, "\ncaught signal: %d\n", a_signal);
		scheduler->f_shutdown(std::bind(&nghttp2::asio_http2::server::http2::stop, std::ref(server)));
	});
	{
		std::string token;
		std::ifstream("session/token") >> token;
		if (!token.empty()) f_refresh(token);
	}
	server.join();
	std::fprintf(stderr, "server stopped.\n");
	return 0;
}
