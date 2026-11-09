#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the driver's sampled W1C acknowledgement and completion selection."""
from pathlib import Path
import re
import sys
src = Path(sys.argv[1]).read_text()
match = re.search(r'static irqreturn_t rga1_irq\([^;{]*\)\s*\{', src)
start = match.start()
end = src.index('\n}\n', start) + 3
body = src[start:end]
Path(sys.argv[2]).write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef int irqreturn_t;
#define BIT(n) (1U<<(n))
#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define RGA1_INT 0
#define RGA1_IRQ_ENABLE 0x700
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
struct rga1_dev { u32 *regs; u32 irq_status, expected_irq; unsigned done; };
static u32 ack;
static u32 readl(u32 *p) { return *p; }
static void writel(u32 v,u32 *p) { ack=v; (void)p; }
static void complete(unsigned *p) { ++*p; }
''' + body + r'''
int main(void)
{
 u32 reg=0;struct rga1_dev r={.regs=&reg,.expected_irq=BIT(2)};
 assert(rga1_irq(0,&r)==IRQ_NONE && !ack && !r.done);
 reg=BIT(3)|0x700;assert(rga1_irq(0,&r)==IRQ_HANDLED);
 assert((ack & 0xf0)==BIT(7) && !r.done && r.irq_status==BIT(3));
 reg=BIT(2)|0x700;rga1_irq(0,&r);
 assert((ack & 0xf0)==BIT(6) && r.done==1 && r.irq_status==(BIT(2)|BIT(3)));
 r.done=r.irq_status=0;r.expected_irq=BIT(3);
 reg=BIT(2);rga1_irq(0,&r);assert(!r.done);
 reg=BIT(3);rga1_irq(0,&r);assert(r.done==1 && (ack & 0xf0)==BIT(7));
 assert((ack & 0x700)==0x700);
 for(unsigned bit=0;bit<2;bit++) {
  r.done=0;reg=BIT(bit);rga1_irq(0,&r);
  assert(r.done==1 && (ack & 0xf0)==BIT(bit+4));
 }
 puts("RGA1 IRQ: sampled W1C, accumulated status, master/passive completion and errors UBSan PASS");
}
''')
