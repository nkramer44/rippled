ARG DIST_TAG=18.04
FROM ubuntu:$DIST_TAG
ARG GIT_COMMIT=unknown
ARG CI_USE=false
LABEL git-commit=$GIT_COMMIT

WORKDIR /root
COPY ubuntu-builder/ubuntu_setup.sh .
RUN ./ubuntu_setup.sh && rm ubuntu_setup.sh

RUN mkdir -m 777 -p /opt/rippled_bld/pkg/
WORKDIR /opt/rippled_bld/pkg

COPY packaging/dpkg/build_dpkg.sh ./
CMD ./build_dpkg.sh
