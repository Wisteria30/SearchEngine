FROM centos:7

RUN yum -y update
RUN yum -y install gcc sqlite sqlite-devel expat-devel bzip2 make
