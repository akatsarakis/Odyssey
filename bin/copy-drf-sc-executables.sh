#!/usr/bin/env bash
HOSTS=( "houston" "sanantonio")
HOSTS=( "houston" "sanantonio" "philly")
HOSTS=( "houston" "sanantonio" "austin")
#HOSTS=( "houston" "austin")
#HOSTS=( "houston" "sanantonio" "austin" "indianapolis")
#HOSTS=( "austin" "houston" "sanantonio")
#HOSTS=( "austin" "houston" "sanantonio" "indianapolis" "philly" )
#HOSTS=( "austin" "houston" "sanantonio" "indianapolis" "philly" "baltimore" "chicago" "atlanta" "detroit")
#HOSTS=( "austin" "houston" "sanantonio" "indianapolis" "chicago" "atlanta" "detroit")
#HOSTS=( "austin" "houston" "sanantonio" "indianapolis" "atlanta")
HOSTS=( "austin" "houston" "sanantonio" "indianapolis")
LOCAL_HOST=`hostname`
EXECUTABLES=("drf-sc" "run-drf-sc.sh")
HOME_FOLDER="/home/s1687259/drf-sc/src/drf-sc"
MAKE_FOLDER="/home/s1687259/drf-sc/src"
DEST_FOLDER="/home/s1687259/drf-sc-exec/src/drf-sc"

cd $MAKE_FOLDER
make clean
make
cd -

for EXEC in "${EXECUTABLES[@]}"
do
	#echo "${EXEC} copied to {${HOSTS[@]/$LOCAL_HOST}}"
	parallel --will-cite scp ${HOME_FOLDER}/${EXEC} {}:${DEST_FOLDER}/${EXEC} ::: $(echo ${HOSTS[@]/$LOCAL_HOST})
	echo "${EXEC} copied to {${HOSTS[@]/$LOCAL_HOST}}"
done
