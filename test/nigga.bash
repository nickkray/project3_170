cd ../
make clean
make
cd test
echo "*****ATTEMPTING TEST $1 BELOW*****************"
../vm/nachos -x $1
