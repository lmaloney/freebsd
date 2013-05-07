# Larry note: Temporarily using this build file, cause vmstat is a system make file, and libstat is a portbuild, need to build new make
# file for vmstat in head, and use the new bsdyml lib... my version of head is missing it, so need to check out something else..
# Note: for this build to work, you need to have libyaml installed from ports, and currently this staticly links in libyaml, need to change..
cc -O2 -pipe -std=gnu99 -fstack-protector -Wsystem-headers -Wno-pointer-sign -c vmstat.c
cc -O2 -pipe -std=gnu99 -fstack-protector -Wsystem-headers -Werror -Wno-pointer-sign -o vmstat vmstat.o emmityaml.o -ldevstat -lkvm -lmemstat -lutil /usr/local/lib/libyaml.a 

