FROM docker.io/ubuntu:24.04

ENV MDBOOK_VERSION=0.4.52

RUN export DEBIAN_FRONTEND=noninteractive  \
    && apt update  \
    && apt install --no-install-recommends -y  \
                   ca-certificates cmake curl doxygen gcc libc6-dev  \
                   libhwloc-dev make pkgconf  \
    && rm -rf /var/lib/apt/lists/*

RUN (curl -L https://github.com/rust-lang/mdBook/releases/download/v${MDBOOK_VERSION}/mdbook-v${MDBOOK_VERSION}-x86_64-unknown-linux-gnu.tar.gz  \
        | tar -xz)  \
    && mv mdbook /usr/local/bin
