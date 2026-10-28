#ifndef WORKLOAD_CPU_H
#define WORKLOAD_CPU_H

#define CPU_ITERATIONS 150000000L

static void cpu_task(int id)
{
volatile double acc = 0.0;
long i;

(void) id;

for (i = 0; i < CPU_ITERATIONS; i++)
acc += (double) i * 1.000001;
}

#endif