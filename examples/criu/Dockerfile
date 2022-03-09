# FROM docker pull ibm-semeru-runtimes:open-11-jdk-criu-beta # or something like that
FROM ubuntu:21.10

ENV LANG='en_US.UTF-8' LANGUAGE='en_US:en' LC_ALL='en_US.UTF-8'

# Compile criu required for checkpoint
RUN apt-get update \
    && apt-get autoremove -y \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
       ca-certificates \
       build-essential asciidoc git vim \
       libprotobuf-dev libprotobuf-c-dev protobuf-c-compiler protobuf-compiler python3-protobuf \
       libbsd-dev libnftables-dev libcap-dev libnl-3-dev libnet-dev \
       pkg-config xmlto python3-distutils libcap2-bin \
       libprotobuf23 libprotobuf-c1 python3-protobuf \
       libbsd0 libnftables1 libnl-3-200 libnet1 dumb-init \
    && cd /tmp \
    && git clone https://github.com/checkpoint-restore/criu.git \
    && cd criu \
    && git fetch --all --tags \
    && git checkout v3.14 \
    && make PREFIX=/usr install \
    && cd / \
    && rm -rf /tmp/criu \
    && setcap cap_sys_time,cap_dac_override,cap_chown,cap_setpcap,cap_setgid,cap_audit_control,cap_dac_read_search,cap_net_admin,cap_sys_chroot,cap_sys_ptrace,cap_fowner,cap_kill,cap_fsetid,cap_sys_resource,cap_setuid,cap_sys_admin=eip /usr/sbin/criu \
    && rm -rf /var/lib/apt/lists/*


# The following may not be needed if we pull an OpenJ9 CRIU image

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        tzdata curl ca-certificates fontconfig locales wget unzip \
    && echo "en_US.UTF-8 UTF-8" >> /etc/locale.gen \
    && locale-gen en_US.UTF-8 \
    && rm -rf /var/lib/apt/lists/*

ARG JDK_DIR=jdk

COPY $JDK_DIR /opt/java/openjdk

ENV JAVA_HOME=/opt/java/openjdk \
    PATH="/opt/java/openjdk/bin:$PATH"


# Install Gradle
RUN wget https://services.gradle.org/distributions/gradle-7.4.1-bin.zip -P /tmp \
    && unzip -d /opt/gradle /tmp/gradle-*.zip \
    && rm -rf /tmp/gradle-*

ENV PATH="$PATH:/opt/gradle/gradle-7.4.1/bin"


# Copy and build application

ARG APP=simpleExample

COPY $APP /opt/$APP

RUN cd /opt/$APP \
    && gradle build \
    && cd /

COPY helpers /opt/helpers 


CMD ["/opt/helpers/runApplication.sh"]
