FROM ubuntu:22.04
ENV BPFTOOL="/usr/lib/linux-tools/6.8.0-40-generic/bpftool"
RUN apt-get update && apt-get install -y \
	clang                                \
	gcc                                  \
	bpfcc-tools                          \
	gcc-aarch64-linux-gnu                \
	gcc-x86-64-linux-gnu                 \
	linux-tools-6.8.0-40-generic         \
	make                                 \
	m4
