#!/bin/sh

IMGNAME=unicam-image

FOURCC=GB10
CAMMEDIA=/dev/media0
if [ -n "$1" ]; then
  CAMMEDIA=$1
fi
if [ "$CAMMEDIA" = "auto" ]; then
	for i in 0 1 2 3 4
	do
		media-ctl -d $i -e $IMGNAME | grep -q "not found"
		if [ $? -eq 1 ]; then
			CAMMEDIA=/dev/media$i
			break
		fi
	done
fi
		
media-ctl -d $CAMMEDIA -e $IMGNAME | grep -q "not found"
if [ $? -eq 0 ]; then
	echo "select another media"
	exit
fi
echo "Media : $CAMMEDIA"

SENSORENTITY=1
SENSORNAME=$(media-ctl -d $CAMMEDIA -p | grep -e "entity $SENSORENTITY:" | sed 's/- entity .*: \(.*\) (.*)/\1/')
IMGENTITY=$(media-ctl -d $CAMMEDIA -p | grep -e "- entity .*: $IMGNAME" | sed 's/- entity \(.*\): .* (.*)/\1/')

SENSOR=$(media-ctl -d $CAMMEDIA -e "$SENSORNAME")
SENSORPADIMG=0
IMAGE=$(media-ctl -d $CAMMEDIA -e "$IMGNAME")
IMGPADSENSOR=0

media-ctl -d $CAMMEDIA -r
# retrieve sensor informations
list_fmt_input=$(v4l2-ctl -d $SENSOR --list-subdev-mbus-codes $SENSORPADIMG | grep -E '0x.*[0-9,a-f]')
fmt_input=$(v4l2-ctl -d $SENSOR --get-subdev-fmt $SENSORPADIMG | grep Mediabus | sed 's/[^ ].*Mediabus Code.*[ ]: 0x.*[0-9,a-f] (MEDIA_BUS_FMT_\(.*\))/\1/')
framesize=$(v4l2-ctl -d $SENSOR --get-subdev-fmt $SENSORPADIMG | grep Width/Height | sed 's,[^ ].*Width/Height.*[ ]: \(.*[0-9]\)/\(.*[0-9]\),\1x\2,')

read -p "current $framesize. Change it (y/N)?" CHOICE
if [ "$CHOICE" = y ]; then
echo $fmt_input
  FOURCC=$(v4l2-ctl -d $IMAGE --list-formats $fmt_input | grep "\[0\]" | sed "s/.*\[0\]: '\(.*\)' .*/\1/")
  v4l2-ctl -d $SENSOR --list-framesizes $FOURCC
  read -p "new framesize : " framesize
fi

OUTIMAGE=$IMAGE

read -p "current fmt $fmt_input. Change it (y/N): " CHOICE
if [ "$CHOICE" = y ]; then
  echo $list_fmt_output
  read -p "set new fmt: " fmt_input
fi

read -p "set $SENSORNAME ($SENSORENTITY) with $fmt_input/$framesize continue (Y/n)?" CHOICE
if [ "$CHOICE" = n ]; then
  exit
fi

# set the link between SENSOR and IMAGE entity
media-ctl -d $CAMMEDIA --set-v4l2 "$SENSORENTITY:$SENSORPADIMG[fmt:$fmt_input/$framesize field:none colorspace:raw]"

media-ctl -d $CAMMEDIA --link "$SENSORENTITY:$SENSORPADIMG->$IMGENTITY:$IMGPADSENSOR[1]"
FOURCC=$(v4l2-ctl -d $OUTIMAGE --list-formats $fmt_input | grep "\[0\]" | sed "s/.*\[0\]: '\(.*\)' .*/\1/")
echo "subdev format code $fmt_input => output fourcc $FOURCC"
read -p "Change fourcc ? [y/N]" CHOICE
if [ "$CHOICE" = "y" ]; then
  I=0
  for fourcc in $(v4l2-ctl -d $OUTIMAGE --list-formats $fmt_input | grep -e "\[.*[0-9]\]" | sed "s/.*\[.*[0-9]\]: '\(.*\)' .*/\1/")
  do
     echo "$I : $fourcc"
     I=$(($I+1))
  done
  read -p "enter your choice: " CHOICE
  if [ -n $CHOICE ]; then
    FOURCC=$(v4l2-ctl -d $OUTIMAGE --list-formats $fmt_input | grep "\[$CHOICE\]" | sed "s/.*\[$CHOICE\]: '\(.*\)' .*/\1/")
  fi
  echo "subdev format code $fmt_input => output fourcc $FOURCC"
fi

WIDTH=$(echo $framesize | sed 's/x[0-9].*//')
HEIGHT=$(echo $framesize | sed 's/[0-9].*x//')
v4l2-ctl -d $OUTIMAGE -v width=$WIDTH,height=$HEIGHT,pixelformat=$FOURCC
while [ $? -ne 0 ]; do
  echo "FourCC $FOURCC for $OUTIMAGE is not available"
  read -p "enter another value: " FOURCC
  if [ -z "$FOURCC" ]; then
    break
  fi
  v4l2-ctl -d $OUTIMAGE -v width=$WIDTH,height=$HEIGHT,pixelformat=$FOURCC
done

setcontrol() {
CTRLS=$(v4l2-ctl -d $1 -l)
echo $CTRLS
}
echo "0: media topology"
echo "1: Image device configuration"
echo "2: try to stream into file"
echo "3: take a picture"
read -p "your choice ? " CHOICE
case "$CHOICE" in
  0)
    media-ctl -d $CAMMEDIA -p
    ;;
  1)
    v4l2-ctl -d $IMAGE -D
    ;;
  2)
    v4l2-ctl --verbose -d $OUTIMAGE --stream-mmap=4 --stream-skip=3 --stream-count=2 --stream-to=tempo.bayer --stream-poll
    ;;
  3)
    v4l2-ctl --verbose -d $OUTIMAGE --stream-mmap --stream-count=1 --stream-to=test.raw
    ;;
  4)
    setcontrol $SENSOR
    ;;
esac
echo Device set to $IMAGE

