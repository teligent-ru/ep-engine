введение по сборке couchbase 3.0.3.teligent ветки под RHEL6 64 bit
==================================================================

все действия от root.

создать виртуалку (virtualbox) 
------------------------------

из образа
http://autobuild.teligent.ru/kickstarts/isos/RHEL/6/rhel-server-6.6-x86_64-dvd.iso
при установке выбрал пункт "Software development workstation".

добавить в /etc/yum.repo.d файлик local64.repo
----------------------------------------------

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

установить пакеты
-----------------

~~~
yum install cmake snappy-devel.x86_64 libicu-devel.x86_64
~~~

установить couchbase-server-3.0.1 
---------------------------------

http://autobuild.teligent.ru/kickstarts/3RD_PARTY/couchbase/RHEL6/x86_64/couchbase-server-community-3.0.1-centos6.x86_64.rpm

(из него возьмётся libv8.so, которую собирать заморочно, а без неё не собирается memcached)

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

#took hash keys from 3.0.3 manifest:
#https://github.com/couchbase/manifest/blob/master/released/3.0.3.xml

g ssh://git@github.com/teligent-ru ep-engine 3.0.3.teligent.1 
g ssh://git@github.com/teligent-ru memcached 3.0.3.teligent.1 #based on 4424e903ad7e44726dc46f73acebd07c960f8e72
g git://github.com/couchbase platform 2a6d25c5cd2b6b7ed0771e15fee941f527a284a9
g ssh://git@github.com/teligent-ru tlm 3.0.3.teligent.1
cp -p tlm/{GNUmakefile,Makefile,CMakeLists.txt} .
g git://github.com/couchbase v8 05120013843918f7e3712159c03b509d3e328cf7
~~~


запустить общую сборку, среди прочего получится ep.so, memcached
----------------------------------------------------------------

~~~
#v8 собирать самому можно, но очень трудно и долго
ln -s /opt/couchbase/lib/libv8.so v8/
#и указываем на уже собранное:
V8_DIR=$PWD/v8 make PREFIX=/opt/couchbase CMAKE_PREFIX_PATH=/opt/couchbase
...
-- Installing: /opt/couchbase/bin/memcached
...
-- Installing: /opt/couchbase/lib/memcached/ep.so
-- Set runtime path of "/opt/couchbase/lib/memcached/ep.so" to "$ORIGIN/../lib:$ORIGIN/../lib/memcached:/opt/couchbase/lib:/opt/couchbase/lib/memcached:/opt/couchbase/lib"
[root@rualpe-vm2 couchbase# 
~~~
