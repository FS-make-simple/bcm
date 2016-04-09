/*

BCM - A BWT-based file compressor
Written and placed in the public domain by Ilya Muravyov

*/

#ifdef __GNUC__
#define _FILE_OFFSET_BITS 64
#define _fseeki64 fseeko64
#define _ftelli64 ftello64
#endif

#define _CRT_SECURE_NO_WARNINGS
#define _CRT_DISABLE_PERFCRIT_LOCKS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "divsufsort.h" // libdivsufsort-lite

typedef unsigned char byte;
typedef unsigned int uint;
typedef unsigned long long ulonglong;

const char magic[]="BCM1";

FILE* in;
FILE* out;

class Encoder
{
public:
	uint code;
	uint low;
	uint high;

	Encoder()
		: code(0), low(0), high(-1)
	{}

	void Encode(int bit, uint p)
	{
		const uint mid=low+((ulonglong(high-low)*(p<<14))>>32);

		if (bit)
			high=mid;
		else
			low=mid+1;

		while ((low^high)<(1<<24))
		{
			putc(low>>24, out);
			low<<=8;
			high=(high<<8)|255;
		}
	}

	void Flush()
	{
		for (int i=0; i<4; ++i)
		{
			putc(low>>24, out);
			low<<=8;
		}
	}

	void Init()
	{
		for (int i=0; i<4; ++i)
			code=(code<<8)|getc(in);
	}

	int Decode(uint p)
	{
		const uint mid=low+((ulonglong(high-low)*(p<<14))>>32);

		const int bit=(code<=mid);
		if (bit)
			high=mid;
		else
			low=mid+1;

		while ((low^high)<(1<<24))
		{
			code=(code<<8)|getc(in);
			low<<=8;
			high=(high<<8)|255;
		}

		return bit;
	}
};

template<int RATE>
class Counter
{
public:
	int p;

	Counter()
		: p(1<<15)
	{}

	void Update(int bit)
	{
		if (bit)
			p+=(p^65535)>>RATE;
		else
			p-=p>>RATE;
	}
};

class CM: public Encoder
{
public:
	Counter<2> counter0[256];
	Counter<4> counter1[256][256];
	Counter<6> counter2[2][256][17];
	int c1;
	int c2;
	int run;

	CM()
		: c1(0), c2(0), run(0)
	{
		for (int i=0; i<2; ++i)
		{
			for (int j=0; j<256; ++j)
			{
				for (int k=0; k<17; ++k)
					counter2[i][j][k].p=(k-(k==16))<<12;
			}
		}
	}

	void Put(int c)
	{
		if (c1==c2)
			++run;
		else
			run=0;
		const int f=(run>2);

		int ctx=1;
		while (ctx<256)
		{
			const int p0=counter0[ctx].p;
			const int p1=counter1[c1][ctx].p;
			const int p2=counter1[c2][ctx].p;
			const int p=(p0+p0+p0+p0+p1+p1+p1+p2)>>3;

			const int idx=p>>12;
			const int x1=counter2[f][ctx][idx].p;
			const int x2=counter2[f][ctx][idx+1].p;
			const int ssep=x1+(((x2-x1)*(p&4095))>>12);

			const int bit=((c&128)!=0);
			c+=c;
			Encoder::Encode(bit, p+ssep+ssep+ssep);

			counter0[ctx].Update(bit);
			counter1[c1][ctx].Update(bit);
			counter2[f][ctx][idx].Update(bit);
			counter2[f][ctx][idx+1].Update(bit);

			ctx+=ctx+bit;
		}

		c2=c1;
		c1=byte(ctx);
	}

	int Get()
	{
		if (c1==c2)
			++run;
		else
			run=0;
		const int f=(run>2);

		int ctx=1;
		while (ctx<256)
		{
			const int p0=counter0[ctx].p;
			const int p1=counter1[c1][ctx].p;
			const int p2=counter1[c2][ctx].p;
			const int p=(p0+p0+p0+p0+p1+p1+p1+p2)>>3;

			const int idx=p>>12;
			const int x1=counter2[f][ctx][idx].p;
			const int x2=counter2[f][ctx][idx+1].p;
			const int ssep=x1+(((x2-x1)*(p&4095))>>12);

			const int bit=Encoder::Decode(p+ssep+ssep+ssep);

			counter0[ctx].Update(bit);
			counter1[c1][ctx].Update(bit);
			counter2[f][ctx][idx].Update(bit);
			counter2[f][ctx][idx+1].Update(bit);

			ctx+=ctx+bit;
		}

		c2=c1;
		return c1=byte(ctx);
	}
} cm;

byte* buf;

void compress(int b)
{
	if (_fseeki64(in, 0, SEEK_END)!=0)
	{
		perror("Fseek failed");
		exit(1);
	}
	const long long flen=_ftelli64(in);
	if (flen<0)
	{
		perror("Ftell failed");
		exit(1);
	}
	if (b>flen)
		b=int(flen);
	rewind(in);

	buf=(byte*)calloc(b, 5);
	if (!buf)
	{
		fprintf(stderr, "Out of memory\n");
		exit(1);
	}

	putc(magic[0], out);
	putc(magic[1], out);
	putc(magic[2], out);
	putc(magic[3], out);

	int n;
	while ((n=fread(buf, 1, b, in))>0)
	{
		const int p=divbwt(buf, buf, (int*)&buf[b], n);
		if (p<1)
		{
			perror("Divbwt failed");
			exit(1);
		}

		cm.Put(n>>24);
		cm.Put(n>>16);
		cm.Put(n>>8);
		cm.Put(n);
		cm.Put(p>>24);
		cm.Put(p>>16);
		cm.Put(p>>8);
		cm.Put(p);

		for (int i=0; i<n; ++i)
			cm.Put(buf[i]);
	}

	cm.Put(0); // EOF
	cm.Put(0);
	cm.Put(0);
	cm.Put(0);

	cm.Flush();
}

void decompress()
{
	if ((getc(in)!=magic[0])
		||(getc(in)!=magic[1])
		||(getc(in)!=magic[2])
		||(getc(in)!=magic[3]))
	{
		fprintf(stderr, "Not in BCM format\n");
		exit(1);
	}

	cm.Init();

	int b=0;

	for (;;)
	{
		const int n=(cm.Get()<<24)
			|(cm.Get()<<16)
			|(cm.Get()<<8)
			|cm.Get();
		if (n==0)
			break;
		if (b==0)
		{
			buf=(byte*)calloc(b=n, 5);
			if (!buf)
			{
				fprintf(stderr, "Out of memory\n");
				exit(1);
			}
		}
		const int p=(cm.Get()<<24)
			|(cm.Get()<<16)
			|(cm.Get()<<8)
			|cm.Get();
		if ((n<1)||(n>b)||(p<1)||(p>n))
		{
			fprintf(stderr, "File corrupted\n");
			exit(1);
		}
		// Inverse BWT
		int t[257]={0};
		for (int i=0; i<n; ++i)
			++t[(buf[i]=cm.Get())+1];
		for (int i=1; i<256; ++i)
			t[i]+=t[i-1];
		int* next=(int*)&buf[b];
		for (int i=0; i<n; ++i)
			next[t[buf[i]]++]=i+(i>=p);
		for (int i=p; i!=0;)
		{
			i=next[i-1];
			putc(buf[i-(i>=p)], out);
		}
	}
}

int main(int argc, char* argv[])
{
	const clock_t start=clock();

	int block_size=20<<20; // 20 MB
	bool do_decomp=false;
	bool overwrite=false;

	while ((argc>1)&&(argv[1][0]=='-'))
	{
		switch (argv[1][1])
		{
		case 'b':
			block_size=atoi(&argv[1][2])
				<<(argv[1][strlen(argv[1])-1]=='k'?10:20);
			if (block_size<1)
			{
				fprintf(stderr, "Invalid block size\n");
				exit(1);
			}
			break;
		case 'd':
			do_decomp=true;
			break;
		case 'f':
			overwrite=true;
			break;
		default:
			fprintf(stderr, "Unknown option: %s\n", argv[1]);
			exit(1);
		}
		--argc;
		++argv;
	}

	if (argc<2)
	{
		fprintf(stderr,
			"BCM - A BWT-based file compressor, v1.00\n"
			"\n"
			"Usage: BCM [options] infile [outfile]\n"
			"\n"
			"Options:\n"
			"  -b<N>[k] Set block size to N MB or KB (default is 20 MB)\n"
			"  -d       Decompress\n"
			"  -f       Force overwrite of output file\n");
		exit(1);
	}

	in=fopen(argv[1], "rb");
	if (!in)
	{
		perror(argv[1]);
		exit(1);
	}

	char ofname[FILENAME_MAX];
	if (argc<3)
	{
		strcpy(ofname, argv[1]);
		if (do_decomp)
		{
			const int p=strlen(ofname)-4;
			if ((p>0)&&(strcmp(&ofname[p], ".bcm")==0))
				ofname[p]='\0';
			else
				strcat(ofname, ".out");
		}
		else
			strcat(ofname, ".bcm");
	}
	else
		strcpy(ofname, argv[2]);

	if (!overwrite)
	{
		FILE* f=fopen(ofname, "rb");
		if (f)
		{
			fclose(f);
			fprintf(stderr, "%s already exists\n", ofname);
			exit(1);
		}
	}

	out=fopen(ofname, "wb");
	if (!out)
	{
		perror(ofname);
		exit(1);
	}

	fprintf(stdout, "%s: ", argv[1]);
	fflush(stdout);

	if (do_decomp)
		decompress();
	else
		compress(block_size);

	fprintf(stdout, "%lld -> %lld in %.3fs\n",
		_ftelli64(in), _ftelli64(out),
		double(clock()-start)/CLOCKS_PER_SEC);

	fclose(in);
	fclose(out);

	free(buf);

	return 0;
}
