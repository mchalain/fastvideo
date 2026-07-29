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
echo "Media : $CAMMEDIA"

CSIENTITY=1
CSINAME=$(media-ctl -d $CAMMEDIA -p | grep -e "entity $CSIENTITY:" | sed 's/- entity .*: \(.*\) (.*)/\1/')
SENSORENTITY=16
SENSORNAME=$(media-ctl -d $CAMMEDIA -p | grep -e "entity $SENSORENTITY:" | sed 's/- entity .*: \(.*\) (.*)/\1/')
FEENTITY=10
FENAME=$(media-ctl -d $CAMMEDIA -p | grep -e "entity $FEENTITY:" | sed 's/- entity .*: \(.*\) (.*)/\1/')
CHNAME=rp1-cfe-csi2_ch0
CHENTITY=$(media-ctl -d $CAMMEDIA -p | grep -e "- entity .*: $CHNAME" | sed 's/- entity \(.*\): .* (.*)/\1/')
IMGNAME=rp1-cfe-fe_image0
IMGENTITY=$(media-ctl -d $CAMMEDIA -p | grep -e "- entity .*: $IMGNAME" | sed 's/- entity \(.*\): .* (.*)/\1/')
CONFNAME=rp1-cfe-fe_config
CONFENTITY=$(media-ctl -d $CAMMEDIA -p | grep -e "- entity .*: $CONFNAME" | sed 's/- entity \(.*\): .* (.*)/\1/')
STATSNAME=rp1-cfe-fe_stats
STATSENTITY=$(media-ctl -d $CAMMEDIA -p | grep -e "- entity .*: $STATSNAME" | sed 's/- entity \(.*\): .* (.*)/\1/')
EMBNAME=rp1-cfe-embedded
EMBENTITY=$(media-ctl -d $CAMMEDIA -p | grep -e "- entity .*: $EMBNAME" | sed 's/- entity \(.*\): .* (.*)/\1/')

SENSOR=$(media-ctl -d $CAMMEDIA -e "$SENSORNAME")
SENSORPADIMG=0
SENSORSTREAM=0
SENSORPADCONF=1
CSI=$(media-ctl -d $CAMMEDIA -e "$CSINAME")
CSIPADSENSOR=0
CSIPADCH=4
CSIPADCONF=1
CSIPADMETA=5
FE=$(media-ctl -d $CAMMEDIA -e "$FENAME")
FEPADIN=0
FEPADCONF=1
FEPADOUT=2
FEPADSTATS=4
IMAGE=$(media-ctl -d $CAMMEDIA -e "$IMGNAME")
CONFIG=$(media-ctl -d $CAMMEDIA -e "$CONFNAME")
CHANNEL=$(media-ctl -d $CAMMEDIA -e "$CHNAME")
EMBEDDED=$(media-ctl -d $CAMMEDIA -e "$EMBNAME")

media-ctl -d $CAMMEDIA -r
# retrieve sensor informations
list_fmt_input=$(v4l2-ctl -d $SENSOR --list-subdev-mbus-codes $SENSORPADIMG | grep -E '0x.*[0-9,a-f]')
fmt_input=$(v4l2-ctl -d $SENSOR --get-subdev-fmt $SENSORPADIMG | grep Mediabus | sed 's/[^ ].*Mediabus Code.*[ ]: 0x.*[0-9,a-f] (MEDIA_BUS_FMT_\(.*\))/\1/')
framesize=$(v4l2-ctl -d $SENSOR --get-subdev-fmt $SENSORPADIMG | grep Width/Height | sed 's,[^ ].*Width/Height.*[ ]: \(.*[0-9]\)/\(.*[0-9]\),\1x\2,')

read -p "current $framesize change (y/N)?" CHOICE
if [ "$CHOICE" = y ]; then
  read -p "new framesize : " framesize
fi

fmt_bayer=$(echo $fmt_input | sed 's/^\([SBGRY]\+\)[0-9]*_.*/\1/')
fmt_depth=$(echo $fmt_input | sed 's/^\([SBGRY]\+\)\([0-9]*\)_.*/\2/')
fmt_input=${fmt_bayer}${fmt_depth}_1X${fmt_depth}
SENSOR_BAYER=$fmt_bayer
read -p "current fmt $fmt_input of $SENSORNAME ($SENSORENTITY). Change it (y/N)?" CHOICE
if [ "$CHOICE" = y ]; then
  read -p "set new fmt: " fmt_input
fi

# the "media-ctl -r" reset above may have reverted the sensor's own mode -
# CSI2's sink pad below only declares what it EXPECTS to receive, it does
# not itself command the sensor to switch mode. Without this, the sensor
# can stay at a different native resolution while CSI2 is told to expect
# $framesize, causing "Wrong width or height ... (remote pad set to ...)"
# at streamon time.
media-ctl -d $CAMMEDIA --set-v4l2 "$SENSORENTITY:$SENSORPADIMG[fmt:$fmt_input/$framesize field:none colorspace:raw]"

# set the link between SENSOR and CSI entity
media-ctl -d $CAMMEDIA --set-v4l2 "$CSIENTITY:$CSIPADSENSOR[fmt:$fmt_input/$framesize field:none colorspace:raw]"

# the output format must be Bayer 16bits for PiSP backend
list_fmt_output=$(v4l2-ctl -d $FE --list-subdev-mbus-codes $FEPADOUT | grep -E '0x.*[0-9,a-f]')
fe_fmt_output=$(v4l2-ctl -d $FE --get-subdev-fmt $FEPADOUT | grep Mediabus | sed 's/[^ ].*Mediabus Code.*[ ]: 0x.*[0-9,a-f] (MEDIA_BUS_FMT_\(.*\))/\1/')

# PiSP-FE's own reported default ($fe_fmt_output) is a generic Bayer guess -
# it has no idea what sensor is actually connected, so for a monochrome
# sensor (SENSOR_BAYER=Y) it still defaults to something like SRGGB16_1X16,
# which the rp1-cfe driver's format table does NOT pair with V4L2_PIX_FMT_Y16
# (only exact code Y16_1X16 does) - "Format mismatch!" at streamon time
# otherwise. Derive the default from the SENSOR's own pattern instead.
echo "pisp-fe reports $fe_fmt_output, deriving from sensor pattern instead:"
fmt_output=${SENSOR_BAYER}16_1X16

read -p "current fmt $fmt_output of $FENAME. Change it (y/N): " CHOICE
if [ "$CHOICE" = y ]; then
  read -p "set new fmt: " fmt_output
fi

media-ctl -d $CAMMEDIA --set-v4l2 "$FEENTITY:$FEPADIN[fmt:$fmt_output/$framesize field:none]"

OUTCSI=$FEENTITY
OUTIMAGE=$IMAGE
read -p "disable the pisp_fe (y/N)" CHOICE
if [ "$CHOICE" = y ]; then
OUTCSI=$CHENTITY
OUTIMAGE=$CHANNEL
fi

echo "fmt image "$fmt_output
media-ctl -d $CAMMEDIA --set-v4l2 "$CSIENTITY:$CSIPADCH[fmt:$fmt_output/$framesize field:none colorspace:raw]"

if [ "$OUTCSI" = "$FEENTITY" ]; then
 # set the image analyzer
 media-ctl -d $CAMMEDIA --set-v4l2 "$OUTCSI:0[fmt:$fmt_output/$framesize field:none colorspace:raw]"
 media-ctl -d $CAMMEDIA --set-v4l2 "$FEENTITY:$FEPADOUT[fmt:$fmt_output/$framesize field:none]"
fi

media-ctl -d $CAMMEDIA --link "$CSIENTITY:$CSIPADCH->$OUTCSI:0[1]"

media-ctl -d $CAMMEDIA --link "$SENSORENTITY:$SENSORPADIMG->$CSIENTITY:$CSIPADSENSOR[1]"
# set the link between SENSOR and CSI for the configuration if it exists
media-ctl -d $CAMMEDIA --get-v4l2 $SENSORENTITY:$SENSORPADCONF | grep "not found" > /dev/null
if [ $? -ne 0 ]; then
  media-ctl -d $CAMMEDIA --link "$SENSORENTITY:$SENSORPADCONF->$CSIENTITY:$CSIPADCONF[1]"
fi

fmt_code=$(v4l2-ctl -d $CSI --get-subdev-fmt $CSIPADCH | grep Mediabus | sed 's/[^ ].*Mediabus Code.*[ ]: \(0x.*[0-9,a-f]\) (MEDIA_BUS_FMT_\(.*\))/\1/')
fmt_codein=$(v4l2-ctl -d $CSI --get-subdev-fmt $CSIPADSENSOR | grep Mediabus | sed 's/[^ ].*Mediabus Code.*[ ]: \(0x.*[0-9,a-f]\) (MEDIA_BUS_FMT_\(.*\))/\1/')
FOURCC=$(v4l2-ctl -d $OUTIMAGE --list-formats $fmt_code | grep "\[0\]" | sed "s/.*\[0\]: '\(.*\)' .*/\1/")
echo "subdev format code $fmt_code/$fmt_codein $fmt_output => output fourcc $FOURCC"
read -p "Change fourcc ? [y/N]" CHOICE
if [ "$CHOICE" = "y" ]; then
  I=0
  for fourcc in $(v4l2-ctl -d $OUTIMAGE --list-formats $fmt_code | grep -e "\[.*[0-9]\]" | sed "s/.*\[.*[0-9]\]: '\(.*\)' .*/\1/")
  do
     echo "$I : $fourcc"
     I=$(($I+1))
  done
  read -p "enter your choice: " CHOICE
  if [ -n $CHOICE ]; then
    FOURCC=$(v4l2-ctl -d $OUTIMAGE --list-formats $fmt_code | grep "\[$CHOICE\]" | sed "s/.*\[$CHOICE\]: '\(.*\)' .*/\1/")
  fi
  echo "subdev format code $fmt_code $fmt_output => output fourcc $FOURCC"
fi

WIDTH=$(echo $framesize | sed 's/x[0-9].*//')
HEIGHT=$(echo $framesize | sed 's/[0-9].*x//')
v4l2-ctl -d $OUTIMAGE -v width=$WIDTH,height=$HEIGHT,pixelformat="$FOURCC"
while [ $? -ne 0 ]; do
  echo "FourCC $FOURCC for $OUTIMAGE is not available"
  read -p "enter another value: " FOURCC
  if [ -z "$FOURCC" ]; then
    break
  fi
  v4l2-ctl -d $OUTIMAGE -v width=$WIDTH,height=$HEIGHT,pixelformat="$FOURCC"
done

if [ "$OUTCSI" = "$FEENTITY" ]; then
 # set the image output
 media-ctl -d $CAMMEDIA --link "$FEENTITY:$FEPADOUT->$IMGENTITY:0[1]"
 # set the configuration device. this must be enabled otherwise Kernel Panic :-(
 media-ctl -d $CAMMEDIA --link "$CONFENTITY:0->$FEENTITY:$FEPADCONF[1]"

 # set the stats device but disable to no be forced to stream with data
 media-ctl -d $CAMMEDIA --link "$FEENTITY:$FEPADSTATS->$STATSENTITY:0[0]"

 enable=0
 read -p "enable the embedded stream (y/N)" CHOICE
 if [ "$CHOICE" = y ]; then
  enable=1
fi
 media-ctl -d $CAMMEDIA --link "$CSIENTITY:$CSIPADMETA->$EMBENTITY:0[$enable]"

 enable=1
 read -p "disable the config stream (y/N)" CHOICE
 if [ "$CHOICE" = y ]; then
  enable=0
 fi
 media-ctl -d $CAMMEDIA --link "$CONFENTITY:0->$FEENTITY:$FEPADCONF[$enable]"

 enable=0
 read -p "enable the stats stream (y/N)" CHOICE
 if [ "$CHOICE" = y ]; then
  enable=1
 fi
 media-ctl -d $CAMMEDIA --link "$FEENTITY:$FEPADSTATS->$STATSENTITY:0[$enable]"
fi

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
esac
echo Device set to $IMAGE

