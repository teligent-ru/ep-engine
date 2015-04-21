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

установить спец-выкачиватель git-репозитариев
---------------------------------------------

~~~
curl https://storage.googleapis.com/git-repo-downloads/repo > ~/bin/repo
chmod +x ~/bin/repo
~~~

выкачать исходники по манифесту
-------------------------------

(он берёт ep-engine и tlb из веток 3.0.1.teligent, которые начались от 3.0.1 версии, подсмотренной в released/3.0.1.xml)
~~~
cd
mkdir couchbase
cd couchbase
~/bin/repo init -u git://github.com/teligent-ru/manifest.git -m released/3.0.1.teligent.xml
repo sync
~~~

запустить общую сборку, среди прочего получится ep.so
-----------------------------------------------------

~~~
#v8 собирать самому можно, но очень трудно и долго
ln -s /opt/couchbase/lib/libv8.so v8/
#и указываем на уже собранное:
V8_DIR=$PWD/v8 make PREFIX=/opt/couchbase CMAKE_PREFIX_PATH=/opt/couchbase
...
-- Installing: /opt/couchbase/lib/memcached/ep.so
-- Set runtime path of "/opt/couchbase/lib/memcached/ep.so" to "$ORIGIN/../lib:$ORIGIN/../lib/memcached:/opt/couchbase/lib:/opt/couchbase/lib/memcached:/opt/couchbase/lib"
[root@rualpe-vm2 couchbase# 
~~~
