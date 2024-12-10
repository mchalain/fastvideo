#!/bin/sh

FOURCC=RG16
#FOURCC=PB1C
CAMMEDIA=/dev/media1
if [ -n "$1" ]; then
  CAMMEDIA=$1
fi
if [ "$CAMMEDIA" = "auto" ]; then
	for i in 0 1 2 3 4
	do
		media-ctl -d $i -e pisp-fe | grep -q "not found"
		if [ $? -eq 1 ]; then
			CAMMEDIA=/dev/media$i
			break
		fi
	done
fi
		
media-ctl -d $CAMMEDIA -e pisp-fe | grep -q "not found"
if [ $? -eq 0 ]; then
	echo "select another media"
	exit
fi

CSIENTITY=1
CSINAME=$(media-ctl -d $CAMMEDIA -p | grep -e "entity $CSIENTITY:" | sed 's/- entity .*: \(.*\) (.*)/\1/')
SENSORENTITY=16
SENSORNAME=$(media-ctl -d $CAMMEDIA -p | grep -e "entity $SENSORENTITY:" | sed 's/- entity .*: \(.*\) (.*)/\1/')
FEENTITY=10
FENAME=$(media-ctl -d $CAMMEDIA -p | grep -e "entity $FEENTITY:" | sed 's/- entity .*: \(.*\) (.*)/\1/')
CHENTITY=19
IMGNAME=rp1-cfe-fe_image0
IMGENTITY=$(media-ctl -d $CAMMEDIA -p | grep -e "- entity .*: $IMGNAME" | sed 's/- entity \(.*\): .* (.*)/\1/')
CONFNAME=rp1-cfe-fe_config
CONFENTITY=$(media-ctl -d $CAMMEDIA -p | grep -e "- entity .*: $CONFNAME" | sed 's/- entity \(.*\): .* (.*)/\1/')
STATSNAME=rp1-cfe-fe_stats
STATSENTITY=$(media-ctl -d $CAMMEDIA -p | grep -e "- entity .*: $STATSNAME" | sed 's/- entity \(.*\): .* (.*)/\1/')

SENSOR=$(media-ctl -d $CAMMEDIA -e "$SENSORNAME")
SENSORPADIMG=0
SENSORSTREAM=0
SENSORPADCONF=1
CSI=/$(media-ctl -d $CAMMEDIA -e "$CSINAME")
CSIPADSENSOR=0
CSIPADCH=4
CSIPADCONF=1
FE=/$(media-ctl -d $CAMMEDIA -e "$FENAME")
FEPADIN=0
FEPADCONF=1
FEPADOUT=2
FEPADSTATS=4
IMAGE=$(media-ctl -d $CAMMEDIA -e "$IMGNAME")

media-ctl -d $CAMMEDIA -r
# retrieve sensor informations
list_fmt_input=$(v4l2-ctl -d $SENSOR --list-subdev-mbus-codes $SENSORPADIMG | grep -E '0x.*[0-9,a-f]')
fmt_input=$(v4l2-ctl -d $SENSOR --get-subdev-fmt $SENSORPADIMG | grep Mediabus | sed 's/[^ ].*Mediabus Code.*[ ]: 0x.*[0-9,a-f] (MEDIA_BUS_FMT_\(.*\))/\1/')
framesize=$(v4l2-ctl -d $SENSOR --get-subdev-fmt $SENSORPADIMG | grep Width/Height | sed 's,[^ ].*Width/Height.*[ ]: \(.*[0-9]\)/\(.*[0-9]\),\1x\2,')

read -p "current $framesize change (y/N)?" CHOICE
if [ "$CHOICE" = y ]; then
  read -p "new framesize : " framesize
fi
read -p "set $SENSORNAME ($SENSORENTITY) with $fmt_input/$framesize continue (Y/n)?" CHOICE
if [ "$CHOICE" = n ]; then
  exit
fi

# set the link between SENSOR and CSI entity
media-ctl -d $CAMMEDIA --set-v4l2 "$CSIENTITY:$CSIPADSENSOR[fmt:$fmt_input/$framesize field:none colorspace:raw]"
media-ctl -d $CAMMEDIA --link "$SENSORENTITY:$SENSORPADIMG->$CSIENTITY:$CSIPADSENSOR[1]"

# set the link between SENSOR and CSI for the configuration if it exists
media-ctl -d $CAMMEDIA --get-v4l2 $SENSORENTITY:$SENSORPADCONF | grep "not found" > /dev/null
if [ $? -ne 0 ]; then
  media-ctl -d $CAMMEDIA --link "$SENSORENTITY:$SENSORPADCONF->$CSIENTITY:$CSIPADCONF[1]"
fi

# the output format must be Bayer 16bits for PiSP backend
list_fmt_output=$(v4l2-ctl -d $FE --list-subdev-mbus-codes $FEPADOUT | grep -E '0x.*[0-9,a-f]')
fmt_output=$(v4l2-ctl -d $FE --get-subdev-fmt $FEPADOUT | grep Mediabus | sed 's/[^ ].*Mediabus Code.*[ ]: 0x.*[0-9,a-f] (MEDIA_BUS_FMT_\(.*\))/\1/')
read -p "current fmt $fmt_output. Change it (y/N): " CHOICE
if [ "$CHOICE" = y ]; then
  echo $list_fmt_output
fi
fmt_output=SBGGR16_1X16
CSIOUT=$FEENTITY
if [ "$fmt_input" = "$fmt_output" ]; then
CSIOUT=$CHENTITY
fi
echo "fmt image "$fmt_output
media-ctl -d $CAMMEDIA --set-v4l2 "$CSIENTITY:$CSIPADCH[fmt:$fmt_output/$framesize field:none colorspace:raw]"
media-ctl -d $CAMMEDIA --set-v4l2 "$CSIOUT:0[fmt:$fmt_output/$framesize field:none colorspace:raw]"
media-ctl -d $CAMMEDIA --link "$CSIENTITY:$CSIPADCH->$CSIOUT:0[1]"

if [ "$CSIOUT" = "$FEENTITY" ]; then
 # set the bayer encoder
 media-ctl -d $CAMMEDIA --set-v4l2 "$FEENTITY:$FEPADOUT[fmt:$fmt_output/$framesize field:none]"

 # set the image output
 media-ctl -d $CAMMEDIA --link "$FEENTITY:$FEPADOUT->$IMGENTITY:0[1]"
 # set the configuration device
 media-ctl -d $CAMMEDIA --link "$CONFENTITY:0->$FEENTITY:$FEPADCONF[1]"

 # set the stats device
 media-ctl -d $CAMMEDIA --link "$FEENTITY:$FEPADSTATS->$STATSENTITY:0[1]"
fi

WIDTH=$(echo $framesize | sed 's/x[0-9].*//')
HEIGHT=$(echo $framesize | sed 's/[0-9].*x//')
v4l2-ctl -d $IMAGE -v width=$WIDTH,height=$HEIGHT,pixelformat=$FOURCC

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
    v4l2-ctl --verbose -d $IMAGE --stream-mmap=4 --stream-skip=3 --stream-count=2 --stream-to=tempo.bayer --stream-poll
    ;;
  3)
    read -p "set pixel format FOURCC (default: $FOURCC): " FOURCC2
    if [ -n "$FOURCC2" ]; then
      FOURCC=$FOURCC2
    fi
    v4l2-ctl --verbose -d $IMAGE --set-fmt-video=width=1456,height=1088,pixelformat=$FOURCC --stream-mmap --stream-count=1 --stream-to=test.raw
    ;;
esac
echo Device set to $IMAGE

