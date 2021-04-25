FROM ubuntu:20.04
COPY . /austin
RUN  cd /austin && autoreconf --install && ./configure && make && make install
