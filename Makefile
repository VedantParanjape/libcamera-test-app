build: main.cpp
	@g++ main.cpp buffer_writer.cpp -o main -I/usr/local/include/libcamera -lcamera -lpthread
	@echo "Building main.cpp"

run: build
	@echo "Running app"
	@./main
