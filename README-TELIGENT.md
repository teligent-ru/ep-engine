введение по сборке couchbase, teligent ветка
============================================

RHEL7
все действия от root

создать виртуалку (virtualbox) 
------------------------------

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

yum install asciidoc ntpdate http://autobuild.teligent.ru/kickstarts/mrepo/7Server-x86_64/updates/Packages/tzdata-2016f-1.el7.noarch.rpm ftp://rpmfind.net/linux/centos/7.3.1611/os/x86_64/Packages/cmake-2.8.12.2-2.el7.x86_64.rpm
cat>/etc/ntp.cfg
server 192.168.2.30
^D
service ntpdate restart

/usr/include/unicode/uvernum.h
поправить, чтобы был такой суффикс
#define U_ICU_VERSION_SUFFIX _44

~~~

сборка community vanilla версии в rpm
-------------------------------------

скачать couchbase-server-enterprise-4.5.1-centos7.x86_64.rpm и установить
залить scp couchbase-server-enterprise-4.5.1-centos7.x86_64.rpm  alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/RHEL7/x86_64/
#теперь доступно http://gigant.teligent.ru/kickstarts/3RD_PARTY/couchbase/RHEL7/x86_64/couchbase-server-enterprise-4.5.1-centos7.x86_64.rpm

mkdir -p ~/rpmbuild/SPECS
cd ~/rpmbuild/SPECS
восстановть спеку rpmrebuild -e couchbase-server-enterprise и подправить couchbase-server -> couchbase-server-community и наоборот в одном месте
залить scp couchbase-server-community-4.5.1.spec alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/SRPM/
#теперь доступно http://gigant.teligent.ru/kickstarts/3RD_PARTY/couchbase/SRPM/couchbase-server-community-4.5.1.spec

mkdir couchbase.4.5.1.RHEL7
cd couchbase.4.5.1.RHEL7
repo init -u git://github.com/couchbase/manifest.git -m released/4.5.1.xml
repo sync
mkdir build
cd build
#был соблазн включить -D COUCHBASE_KV_COMMIT_VALIDATION=1, 
#но тогда не собирался ns_server
#и было нужно возиться
#vim /opt/couchbase/bin/couchbase-server
#поправить 
#SOFTWARE_VERSION=4.5.1-2844
#ENTERPRISE=false

cmake -G "Unix Makefiles" -D PRODUCT_VERSION:STRING="4.5.1-2844" -D BUILD_ENTERPRISE:BOOL=false -D CMAKE_BUILD_TYPE=RelWithDebInfo -D CMAKE_INSTALL_PREFIX=/opt/couchbase ..

make install
-- Installing: /opt/couchbase/lib/fts/static-bleve-mapping/partials/analysis/tokenizers.html
-- Installing: /opt/couchbase/lib/fts/static-bleve-mapping/partials/analysis/wordlist.html
-- Installing: /opt/couchbase/lib/fts/static-bleve-mapping/partials/analysis/wordlists.html
-- Installing: /opt/couchbase/lib/fts/static-bleve-mapping/partials/mapping
-- Installing: /opt/couchbase/lib/fts/static-bleve-mapping/partials/mapping/index-mapping.html
-- Installing: /opt/couchbase/lib/fts/static-bleve-mapping/partials/mapping/type-mapping-tree.html
-- Installing: /opt/couchbase/lib/fts/static-bleve-mapping/partials/mapping/type-mapping.html
[root@rualpe-vm1 build]# 

cd /
mkdir -p ~/rpmbuild/SOURCES
tar -czvf ~/rpmbuild/SOURCES/couchbase-server-enterprise-to-community-4.5.1-centos7.x86_64.tgz /etc/init.d/couchbase-server /usr/lib/systemd/system/couchbase-server.service /opt/couchbase

cd ~/rpmbuild/SPECS
rpmbuild -bs couchbase-server-community-4.5.1.spec #если будет упираться, chown root:root на все файлы о которых ругань
rpmbuild -bb couchbase-server-community-4.5.1.spec

залить 
scp /root/rpmbuild/SRPMS/couchbase-server-community-4.5.1-2844.src.rpm  alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/SRPM/
scp /root/rpmbuild/RPMS/x86_64/couchbase-server-community-4.5.1-2844.x86_64.rpm alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/RHEL7/x86_64/
будет доступно
http://gigant.teligent.ru/kickstarts/3RD_PARTY/couchbase/RHEL7/x86_64/couchbase-server-community-4.5.1-2844.x86_64.rpm

выкачать исходники
-------------------------------

~~~
cd
mkdir couchbase.4.5.1.teligent.RHEL7
cd couchbase.4.5.1.teligent.RHEL7

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
#https://github.com/couchbase/manifest/blob/master/released/4.5.1.xml

g ssh://git@github.com/teligent-ru ep-engine 4.1.5.teligent.10
#g ssh://git@github.com/teligent-ru platform 4.1.0.teligent.7
g git://github.com/couchbase platform 349e6e8a62bc383967a81fd8cf12e53b0d760d3e
#g ssh://git@github.com/teligent-ru tlm 4.1.0.teligent.7
g git://github.com/couchbase tlm f515995bab8229b88bcb15e05c71dd815029aa4c
\cp -p tlm/CMakeLists.txt .
#g git://github.com/couchbase v8 05120013843918f7e3712159c03b509d3e328cf7 #нет в manifest 4.1.0, пока оставил, неясно, как так
g git://github.com/couchbase memcached 3185fbb2a8a6ec126bf924eaf94e7d52e8a2da8e
g git://github.com/couchbase couchstore bce1f234b312f24968643e55f821bd75327cfc60
g git://github.com/couchbase forestdb 0d24efb9f5aed4c19ba5e66256e3b5b53190a874
~~~

Уберите эти ADD_SUBDIRECTORY из корневого CMakeList.txt
  add_subdirectory given source "googletest" which is not an existing
  add_subdirectory given source "subjson" which is not an existing directory.
  add_subdirectory given source "sigar" which is not an existing directory.
  add_subdirectory given source "moxi" which is not an existing directory.

В platform уберите тесты
#ADD_SUBDIRECTORY(tests)
Уберите механизмы сборки memcached и couchstore, сами по себе эти модули не нужны. Нужны только их заголовочные файлы:
~~~
[root@rualpe-vm1 couchbase.4.5.1.teligent.RHEL7]# cat>memcached/CMakeLists.txt
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
[root@rualpe-vm1 couchbase.4.5.1.teligent.RHEL7]# cat>couchstore/CMakeLists.txt
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

[root@rualpe-vm1 couchbase.4.5.1.teligent.RHEL7]# 
~~~

запустить общую сборку, среди прочего получится ep.so, libcJSON
--------------------------------------------------------------------------

~~~
#make build/Makefile EXTRA_CMAKE_OPTIONS='-G "Unix Makefiles" -D CMAKE_BUILD_TYPE=RelWithDebInfo -D CMAKE_INSTALL_PREFIX=/opt/couchbase'

mkdir build
cd build
cmake -G "Unix Makefiles" -D CMAKE_BUILD_TYPE=RelWithDebInfo -D CMAKE_INSTALL_PREFIX=/opt/couchbase -D COUCHBASE_KV_COMMIT_VALIDATION=1 ..
cd ep-engine
make -j6 install
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
a2x --doctype manpage --format manpage ep-engine/cb.asciidoc -D /usr/share/man/man1/
\cp -p ep-engine/management/cbepctl /opt/couchbase/lib/python/cbepctl
tar -czvf ~/couchbase-4.1.0-patch-to-4.1.0.teligent.7-centos$os.x86_64.tgz /opt/couchbase/lib/{memcached/ep.so,libcJSON*} /opt/couchbase/lib/python/cbepctl /usr/share/man/man1/cb.1
scp ~/couchbase-4.1.0-patch-to-4.1.0.teligent.7-centos$os.x86_64.tgz  alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/RHEL$os/x86_64/
~~~

ссылка для скачивания
---------------------
http://gigant.teligent.ru/kickstarts/3RD_PARTY/couchbase/RHEL7/x86_64/couchbase-4.1.0-patch-to-4.1.0.teligent.7-centos7.x86_64.tgz

установка патча
---------------
tar vxzf ~/couchbase-4.1.0-patch-to-4.1.0.teligent.7-centos7.x86_64.tgz -C /

сводная инструкция на конечном узле
-----------------------------------
man cb


merge изменений из couchbase
----------------------------
cd ep-engine
git remote add couchbase https://github.com/couchbase/ep-engine.git
git pull couchbase <имя branch, который хотите подтянуть; пока не разбирался, как подтягивать в ветке до определённого commit, было нужно всю 4.5.1 только, в ней последний commit совпал с manifest>
#manifest: https://github.com/couchbase/manifest/blob/master/released/4.5.1.xml, искать ep-engine, смотреть upstream:
# <project groups="kv" name="ep-engine" revision="e9a655b49393e1302bf75aa759b11969545c986a" upstream="4.5.1"/>

сборка rpm
==========

установить couchbase-server-4.5.1
---------------------------------

http://autobuild.teligent.ru/kickstarts/3RD_PARTY/couchbase/RHEL7/x86_64/couchbase-server-community-4.5.1-centos7.x86_64.rpm
