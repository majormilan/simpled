# simpled version
VERSION = 0.1

# paths: units are searched in /etc/simpled/system and PREFIX/lib/simpled/system
PREFIX = /usr/local

CC = cc
CPPFLAGS = -D_DEFAULT_SOURCE -DVERSION=\"$(VERSION)\" -DPREFIX=\"$(PREFIX)\"
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Os
LDFLAGS =

# miviex: static and against musl
#CC = musl-gcc
#LDFLAGS = -static -s
