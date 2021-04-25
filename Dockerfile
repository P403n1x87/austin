FROM  ubuntu:20.04
COPY  . /austin
RUN   apt-get install -y autoconf && \
      cd /austin && \
      autoreconf --install && \
      ./configure && \
      make && \
      make install
