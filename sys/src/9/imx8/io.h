enum {
	IRQfiq		= -1,

	PPI		= 16,
	SPI		= 32,

	IRQcntps	= PPI+13,
	IRQcntpns	= PPI+14,

	IRQuart1	= SPI+26,
	IRQuart2	= SPI+27,
	IRQuart3	= SPI+28,
	IRQuart4	= SPI+29,

	IRQi2c1		= SPI+35,
	IRQi2c2		= SPI+36,
	IRQi2c3		= SPI+37,
	IRQi2c4		= SPI+38,

	IRQrdc		= SPI+39,

	IRQusb1		= SPI+40,
	IRQusb2		= SPI+41,

	IRQsctr0	= SPI+47,
	IRQsctr1	= SPI+48,

	IRQenet1	= SPI+118,
};

#define BUSUNKNOWN (-1)
