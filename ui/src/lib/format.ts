/** samples → ms 顯示(統一 2 位小數;null 或 rate<=0 = "—") */
export function samplesToMs(n: number | null | undefined, rate: number): string {
  return n == null || rate <= 0 ? "—" : ((n / rate) * 1000).toFixed(2);
}
