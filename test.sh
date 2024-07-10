sudo rm -f /dev/disco
sudo mknod /dev/disco c 61 0
sudo chmod a+rw /dev/disco

sudo rmmod disco

make
sudo insmod disco.ko
make clean
