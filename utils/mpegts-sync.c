#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "log.h"

typedef struct MPEGHeader_s MPEGHeader_t;
struct MPEGHeader_s
{
	uint8_t sync;
	uint16_t pid2:5;
	uint16_t prio:1;
	uint16_t pusi:1;
	uint16_t tei:1;
	uint16_t pid:8;
	uint8_t cc:4;
	uint8_t afi:2;
	uint8_t tsi:2;
};

void analizepacket(const void *packet, ssize_t length)
{
	MPEGHeader_t *header = (MPEGHeader_t *)packet;
	printf("packet header:\n");
	printf("\tSync %#.02x\n", header->sync);
	printf("\tpid %#.02x\n", header->pid);
	printf("\tcc %u\n", header->cc);
	printf("\tadaptation field %s\n", header->afi & 0x02?"present":"");
	if (header->afi & 0x02)
	{
		uint8_t *adaptfield = ((uint8_t *)packet) + sizeof(MPEGHeader_t);
		unsigned char adaptlength = adaptfield[0];
		printf("\t\tlength %d\n",adaptlength);
		printf("\t\tpcr %s\n",adaptfield[1] & 0x10?"present":"");
		if (adaptfield[1] & 0x10)
		{
			uint32_t pcr = 0;
			pcr |= (adaptfield[2] << 25);
			pcr |= (adaptfield[3] << 17);
			pcr |= (adaptfield[4] << 9);
			pcr |= (adaptfield[5] << 1);
			pcr |= ((adaptfield[6] & 0x7f) >> 7);
			printf("\t\t\tpcr %lu\n", pcr);
			if (adaptlength > 8)
				printf("\t\tpadding last %#.02x\n", adaptfield[adaptlength - 1]);
		}
	}
	printf("\tpes %s\n", header->pusi?"present":"");
}
void printpacket(const unsigned char *packet, ssize_t length)
{
	for (int i = 0; i < length; i++)
	{
		if (i % 16 == 0)
			printf("%.03d >", i);
		if (packet[i] == 0x47)
			printf("\x1B[31m");
		if (packet[i] == 0)
			printf("0x00 ");
		else
			printf("%#.02x ", packet[i]);
		if (packet[i] == 0x47)
			printf("\x1B[0m");
		if (i % 16 == 15)
			printf("< %.03d\n", i);
	}
	printf("< %d\n",length);
}

int main(int argc, char *argv[])
{
	int packet_size = 188;
	if (argc < 2)
		return -1;

	int packetcounter = 0;
	int fd = open(argv[1], O_RDONLY);
	if (fd > 0)
	{
		unsigned char packet[2][204];
		size_t offset = 0;
		size_t length = 0;
		int packetid = 0;
		do {
			length = read(fd, packet[packetid], packet_size);

			packetcounter++;
			for (int i = 0; i < length; i++)
			{
				if (packet[packetid][offset] != 0x47 && packet[packetid][i] == 0x47)
				{
					if (offset != i)
					{
						int tmpid = (packetid + 1) % (sizeof(packet) / sizeof(*packet));
						err("sync %d move of %d bytes (%d %d)", packetcounter, i - offset, packetid, tmpid);
						printpacket(packet[tmpid], length);
						warn("followed by %lu bytes", length);
						printpacket(packet[packetid], length);
						warn("end %lu %d", offset, i);
						analizepacket(packet[tmpid] + offset, length - offset);
						//length = 0;
					}
					offset = i;
					break;
				}
			}
			//warn("sync %d found at %lu", packetcounter, offset);
			packetid ++;
			packetid %= sizeof(packet) / sizeof(*packet);
		} while (length > 0);
		close(fd);
	}
	return 0;
}
