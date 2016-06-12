# Alexa Voice Service Agent

Alexa Voice Service Agent is a tiny client program for [Alexa Voice Service](https://developer.amazon.com/public/solutions/alexa/alexa-voice-service).
It is written in C++.


## Dependencies

* [OpenAL](http://openal.org)
* [FFmpeg](http://www.ffmpeg.org)
* [Boost](http://www.boost.org)
* [nghttp2](https://nghttp2.org) --enable-asio-lib is required.
* [Simple-WebSocket-Server](https://github.com/eidheim/Simple-WebSocket-Server)
* [PicoJSON](https://github.com/kazuho/picojson)
* [Material Design Lite](https://getmdl.io)


## How to Build

    autoreconf -is
    ./configure
    make
    make install


## How to Setup

### Register the Product with Alexa Voice Service

Follow the Step 1 in the [Getting Started](https://developer.amazon.com/public/solutions/alexa/alexa-voice-service/getting-started-with-the-alexa-voice-service) page.

### Configure the Product

Make a private key and a certificate for accepting SSL/TLS connections.

Prepare sound files for alerts.

Make a directiory for session files.

    mkdir session

Edit session.json appropriately.

    cp configuration/example.json configuration/session.json
    vi configuration/session.json

### Authorize the Product via Login With Amazon

Open the URL of the product (e.g. https://localhost:3000 as in example.json) with a web browser.
You will be redirected to the Login With Amazon page.

Log in to Login With Amazon and authorize the product.
You will be redirected back to the product and the console page will appear.


## TODO

* Separete the configuration into provider-side and user-side.
* More instructions.


## License

The MIT License (MIT)

Copyright (c) 2016 Shin-ichi MORITA

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
