FROM centos:6

COPY coreseek-3.2.14 /usr/local/src/coreseek-3.2.14

RUN yum -y update && yum install -y gcc  gcc+ gcc-c++ autoconf  automake make libtool expat-devel* mysql mysql-devel 

RUN cd /usr/local/src/coreseek-3.2.14/mmseg-3.2.14 \
    && ./bootstrap \
    && ./configure --prefix=/usr/local/mmseg3 \
    && make && make install

RUN cd /usr/local/src/coreseek-3.2.14/csft-3.2.14 \
    && sh buildconf.sh \
    && ./configure --prefix=/usr/local/coreseek \
        --without-unixodbc \
        --with-mmseg \
        --with-mmseg-includes=/usr/local/mmseg3/include/mmseg/ \
        --with-mmseg-libs=/usr/local/mmseg3/lib/ --with-mysql \
    && make && make install

# 复制本地配置文件到容器，
COPY ./etc/csft.conf /usr/local/coreseek/etc/csft.conf

VOLUME ["/usr/local/coreseek/etc", "/usr/local/coreseek/var/log"]

EXPOSE 9312

COPY ./entrypoint.sh /
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]