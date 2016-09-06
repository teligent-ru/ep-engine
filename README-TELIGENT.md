введение по сборке couchbase, teligent ветка
============================================

все действия от root.

создать виртуалку (virtualbox) 
------------------------------

RHEL6
-----
из образа
http://autobuild.teligent.ru/kickstarts/isos/RHEL/6/rhel-server-6.6-x86_64-dvd.iso
при установке выбрал пункт "Software development workstation".

добавить в /etc/yum.repo.d файлик local64.repo
~~~
[root@rualpe-vm2 v8]# cat /etc/yum.repos.d/local64.repo 
[local64]
name=Red Hat Enterprise Linux  -  - Local64
baseurl=http://autobuild.teligent.ru/kickstarts/redhat/rhel/6/os/x86_64/Server/
enabled=1
gpgcheck=0

[opt]
name=opt
baseurl=http://autobuild.teligent.ru/kickstarts/redhat/rhel/6/optional/x86_64
enabled=1
gpgcheck=0
[root@rualpe-vm2 v8]# 
~~~


RHEL7
-----
http://autobuild.teligent.ru/kickstarts/isos/RHEL/7/rhel-server-7.0-x86_64-dvd.iso
при установке выбрал пункт "Basic development workstation".

добавить в /etc/yum.repo.d файлик local64.repo
~~~
[root@rualpe-vm1 v8]# cat /etc/yum.repos.d/local64.repo 
[local64]
name=Red Hat Enterprise Linux  -  - Local64
baseurl=http://autobuild.teligent.ru/kickstarts/redhat/rhel/7.0/os/x86_64/
enabled=1
gpgcheck=0

[opt]
name=opt
baseurl=http://autobuild.teligent.ru/kickstarts/redhat/rhel/7/optional/x86_64/
enabled=1
gpgcheck=0
~~~


установить пакеты
-----------------

~~~
yum install cmake snappy-devel.x86_64 libicu-devel.x86_64  openssl-devel.x86_64 libevent-devel.x86_64

/usr/include/unicode/uvernum.h
поправить, чтобы был такой суффикс
#define U_ICU_VERSION_SUFFIX _44

~~~

установить couchbase-server-4.1.0 
---------------------------------

(из него возьмутся libv8.so и libcouchstore.so, которые собирать заморочно, а без них не собирается ep.so)

RHEL6
-----
http://autobuild.teligent.ru/kickstarts/3RD_PARTY/couchbase/RHEL6/x86_64/couchbase-server-community-4.1.0-centos6.x86_64.rpm

RHEL7
----
http://autobuild.teligent.ru/kickstarts/3RD_PARTY/couchbase/RHEL7/x86_64/couchbase-server-community-4.1.0-centos7.x86_64.rpm

выкачать исходники
-------------------------------

~~~
cd
mkdir couchbase
cd couchbase

function g {
	url="$1"
	folder="$2"
	version="$3"
	git clone "$url/$folder"
	cd "$folder"
	git checkout "$version"
	cd ..
}

#ключи официальных релизов можно подсматривать в manifest:
#https://github.com/couchbase/manifest/blob/master/released/3.0.1.xml
#https://github.com/couchbase/manifest/blob/master/released/3.0.3.xml
#https://github.com/couchbase/manifest/blob/master/released/4.0.0.xml
#https://github.com/couchbase/manifest/blob/master/released/4.1.0.xml

g ssh://git@github.com/teligent-ru ep-engine 4.1.0.teligent.7
g ssh://git@github.com/teligent-ru platform 4.1.0.teligent.7
g ssh://git@github.com/teligent-ru tlm 4.1.0.teligent.7
\cp -p tlm/{GNUmakefile,Makefile,CMakeLists.txt} .
g git://github.com/couchbase v8 05120013843918f7e3712159c03b509d3e328cf7 #нет в manifest 4.1.0, пока оставил, неясно, как так
g git://github.com/couchbase memcached 7d88d0bba08f07c667ef167f79d3a81dc5f5a825
g git://github.com/couchbase couchstore 7d6bc22a15b80f1da6dd169a3e538e985db1b91a
~~~

Уберите механизмы сборки memcached и couchstore, сами по себе эти модули не нужны. Нужны только их заголовочные файлы:
~~~
[root@rualpe-vm1 couchbase.4.0.0.RHEL7]# cat memcached/CMakeLists.txt
PROJECT(Memcached)
CMAKE_MINIMUM_REQUIRED(VERSION 2.8)

IF (${CMAKE_MAJOR_VERSION} GREATER 2)
    CMAKE_POLICY(SET CMP0042 NEW)
ENDIF (${CMAKE_MAJOR_VERSION} GREATER 2)

INCLUDE(CheckCSourceCompiles)
INCLUDE(CheckIncludeFiles)
INCLUDE(CheckIncludeFileCXX)

INCLUDE_DIRECTORIES(BEFORE
                    ${CMAKE_INSTALL_PREFIX}/include
                    ${CMAKE_CURRENT_SOURCE_DIR}/include
                    ${CMAKE_CURRENT_BINARY_DIR}
                    ${CMAKE_CURRENT_SOURCE_DIR})

[root@rualpe-vm1 couchbase.4.0.0.RHEL7]# cat couchstore/CMakeLists.txt
PROJECT(Couchstore)
CMAKE_MINIMUM_REQUIRED(VERSION 2.8)

IF (${CMAKE_MAJOR_VERSION} GREATER 2)
    CMAKE_POLICY(SET CMP0042 NEW)
ENDIF (${CMAKE_MAJOR_VERSION} GREATER 2)

INCLUDE_DIRECTORIES(BEFORE ${CMAKE_INSTALL_PREFIX}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/src
                           ${CMAKE_CURRENT_BINARY_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}
                           ${Platform_SOURCE_DIR}/include)

[root@rualpe-vm1 couchbase.4.0.0.RHEL7]#
~~~

запустить общую сборку, среди прочего получится ep.so, libcJSON
--------------------------------------------------------------------------

~~~
#(было лень переименовывать папку, не обращайте внимания)
[root@rualpe-vm1 couchbase.4.0.0.RHEL7]#  make PREFIX=/opt/couchbase CMAKE_PREFIX_PATH=/opt/couchbase EXTRA_CMAKE_OPTIONS='-D CMAKE_BUILD_TYPE=RelWithDebInfo'
...
Install the project...
-- Install configuration: "RelWithDebInfo"
-- Up-to-date: /opt/couchbase/lib/libcJSON.so.1.0.0
-- Up-to-date: /opt/couchbase/lib/libcJSON.so
-- Up-to-date: /opt/couchbase/lib/libJSON_checker.so.1.0.0
-- Up-to-date: /opt/couchbase/lib/libJSON_checker.so
-- Up-to-date: /opt/couchbase/lib/libplatform.so.0.1.0
-- Up-to-date: /opt/couchbase/lib/libplatform.so
-- Up-to-date: /opt/couchbase/lib/libdirutils.so.0.1.0
-- Up-to-date: /opt/couchbase/lib/libdirutils.so
-- Up-to-date: /opt/couchbase/lib/memcached/ep.so
[root@rualpe-vm1 couchbase.4.0.0.RHEL7]#
~~~


выложить результат
------------------

~~~
os=7
\cp -p ./ep-engine/management/cbepctl /opt/couchbase/lib/python/cbepctl
tar -czvf ~/couchbase-4.1.0-patch-to-4.1.0.teligent.7-centos$os.x86_64.tgz /opt/couchbase/lib/{memcached/ep.so,libcJSON*} /opt/couchbase/lib/python/cbepctl
scp ~/couchbase-4.1.0-patch-to-4.1.0.teligent.7-centos$os.x86_64.tgz  alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/RHEL$os/x86_64/
~~~

ссылка для скачивания
---------------------
http://gigant.teligent.ru/kickstarts/3RD_PARTY/couchbase/RHEL7/x86_64/couchbase-4.1.0-patch-to-4.1.0.teligent.7-centos7.x86_64.tgz

установка патча
---------------
tar vxzf ~/couchbase-4.1.0-patch-to-4.1.0.teligent.7-centos7.x86_64.tgz -C /
