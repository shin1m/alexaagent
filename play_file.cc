#include <chrono>
#include <memory>
#include <thread>
#include <cstdio>

#include "audio.h"

int main(int argc, char* argv[])
{
	if (argc != 2) return -1;
	av_register_all();
	std::unique_ptr<ALCdevice, decltype(&alcCloseDevice)> device(alcOpenDevice(NULL), alcCloseDevice);
	if (!device) throw std::runtime_error("alcOpenDevice");
	std::unique_ptr<ALCcontext, void (*)(ALCcontext*)> context(alcCreateContext(device.get(), NULL), [](auto a_x)
	{
		alcMakeContextCurrent(NULL);
		alcDestroyContext(a_x);
	});
	alcMakeContextCurrent(context.get());
	alGetError();
	std::unique_ptr<std::FILE, decltype(&std::fclose)> fp(std::fopen(argv[1], "r"), std::fclose);
	if (!fp) throw std::runtime_error("fopen");
	t_callback_audio_source source(std::bind(std::fread, std::placeholders::_1, 1, std::placeholders::_2, fp.get()));
	t_audio_decoder decoder(source);
	t_audio_target target;
	try {
		decoder([&](size_t a_channels, size_t a_bytes, const char* a_p, size_t a_n, size_t a_rate)
		{
			target(a_channels, a_bytes, a_p, a_n, a_rate);
			std::fprintf(stderr, "%.3f - %.3f\r", target.f_offset(), target.f_remain());
			std::this_thread::sleep_for(std::chrono::duration<double>(target.f_remain() * 0.25));
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
