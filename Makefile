CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall
SRC = $(wildcard src/*.cpp)

servex: $(SRC)
	$(CXX) $(CXXFLAGS) -o servex $(SRC)

test: servex
	python3 tests/test_servex.py
	python3 tests/test_http_core.py
	python3 tests/test_conditional_range.py
	python3 tests/test_admin.py
	python3 tests/test_vhost.py
	python3 tests/test_middleware.py
	python3 tests/test_ratelimit.py
	python3 tests/test_proxy.py
	python3 tests/test_cgi.py
	python3 tests/test_websocket.py
	python3 tests/test_logrotate.py

clean:
	rm -f servex

.PHONY: test clean
