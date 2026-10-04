FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        gdb \
        strace \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/

WORKDIR /app

CMD ["/bin/bash"]
