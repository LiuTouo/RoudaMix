/** 對齊 start_scan reply 與可能先到的 progress/done event。 */
export function matchScanJob(
  currentJobId: number | null,
  requestPending: boolean,
  eventJobId: number,
): { matches: boolean; jobId: number | null } {
  if (currentJobId === eventJobId) return { matches: true, jobId: currentJobId };
  if (requestPending && currentJobId === null)
    return { matches: true, jobId: eventJobId };
  return { matches: false, jobId: currentJobId };
}
