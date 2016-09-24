#include <thread>

#include "audio.h"
#include "tiny_http.h"

t_audio_source* f_open(const char* a_url)
{
	std::string url = a_url;
	while (true) {
		std::fprintf(stderr, "opening: %s\n", url.c_str());
		try {
			return new t_url_audio_source(url.c_str());
		} catch (std::exception& e) {
			std::fprintf(stderr, "caught: %s\ntrying to resolve...\n", e.what());
			boost::asio::io_service io;
			t_http10 http(url);
			t_audio_source* source = nullptr;
			http("GET")(io, [&](auto& a_socket)
			{
				if (http.v_code != 200) throw std::runtime_error("invalid code");
				std::smatch match;
				std::regex content_type{"Content-Type:\\s*\\S+/([^\\s;]+)\\s*;?.*\r"};
				for (auto& x : http.v_headers) if (std::regex_match(x, match, content_type)) break;
				if (match.empty()) throw std::runtime_error("no Content-Type found");
				boost::system::error_code ec;
				if (match[1] == "x-mpegurl") {
					std::fprintf(stderr, "found x-mpegurl.\n");
					boost::asio::read_until(a_socket, http.v_buffer, '\n', ec);
					if (ec && ec != boost::asio::error::eof) throw ec;
					std::string line;
					std::getline(std::istream(&http.v_buffer), line);
					if (!std::regex_match(line, match, std::regex{"\\s*(https?://\\S+)\\s*\r?"})) throw std::runtime_error("invalid url");
					url = match[1];
				} else if (match[1] == "x-scpls") {
					std::fprintf(stderr, "found x-scpls.\n");
					boost::asio::read(a_socket, http.v_buffer, ec);
					if (ec && ec != boost::asio::error::eof) throw ec;
					auto buffer = std::make_shared<boost::asio::streambuf>();
					std::ostream stream(buffer.get());
					stream <<
					"#EXTM3U\n"
					"#EXT-X-TARGETDURATION:0\n";
					std::regex file{"File\\d+\\s*=\\s*(https?://\\S+)\\s*\r?"};
					std::string line;
					while (std::getline(std::istream(&http.v_buffer), line))
						if (std::regex_match(line, match, file)) stream << "#EXTINF:0\n" << match[1] << '\n';
					stream << "#EXT-X-ENDLIST\n";
					source = new t_callback_audio_source([buffer](auto a_p, auto a_n)
					{
						return buffer->sgetn(reinterpret_cast<char*>(a_p), a_n);
					});
				} else {
					throw std::runtime_error("unknown Content-Type: " + match[1].str());
				}
			});
			if (source) return source;
		}
	}
}

int main(int argc, char* argv[])
{
	if (argc != 2) return -1;
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
	std::unique_ptr<t_audio_source> source(f_open(argv[1]));
	t_audio_decoder decoder(*source);
	t_audio_target target;
	try {
		std::fprintf(stderr, "decoding...\n");
		decoder([&](size_t a_channels, size_t a_bytes, const char* a_p, size_t a_n, size_t a_rate)
		{
			auto queued = target(a_channels, a_bytes, a_p, a_n, a_rate);
			std::fprintf(stderr, "%.3f - %.3f\r", target.f_offset(), target.f_remain());
			while (queued > 32) {
				std::this_thread::sleep_for(std::chrono::duration<double>(target.f_remain() * 0.5));
				queued = target.f_flush();
			}
		});
	} catch (std::exception& e) {
		std::fprintf(stderr, "caught: %s\n", e.what());
	}
	while (target.f_flush() > 0) {
		std::fprintf(stderr, "%.3f - %.3f\r", target.f_offset(), target.f_remain());
		std::this_thread::sleep_for(std::chrono::duration<double>(target.f_remain() * 0.25));
	}
	return 0;
}
