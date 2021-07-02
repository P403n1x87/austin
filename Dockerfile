FROM  ubuntu:20.04
COPY  . /austin
RUN   apt-get update && \
      apt-get install -y autoconf build-essential && \
      cd /austin && \
      autoreconf --install && \
      ./configure && \
      make && \
      make install
