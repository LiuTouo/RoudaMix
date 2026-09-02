/**
 * 在同一 lane 內把 sourceTrackId 插到 targetPosition，並回傳新的 master 順序。
 * targetPosition 使用移除前的插入槽語意：[0, lane.length]。
 */
export function reorderLane(
  masterTrackIds: readonly number[],
  laneTrackIds: readonly number[],
  sourceTrackId: number,
  targetPosition: number,
): number[] {
  const unchanged = () => [...masterTrackIds];
  if (laneTrackIds.length === 0 || !laneTrackIds.includes(sourceTrackId)) return unchanged();
  if (new Set(masterTrackIds).size !== masterTrackIds.length) return unchanged();
  if (new Set(laneTrackIds).size !== laneTrackIds.length) return unchanged();
  if (laneTrackIds.some((id) => !masterTrackIds.includes(id))) return unchanged();

  const position = Math.max(0, Math.min(Math.trunc(targetPosition), laneTrackIds.length));
  const sourceIndex = masterTrackIds.indexOf(sourceTrackId);
  const targetIndex =
    position === laneTrackIds.length
      ? masterTrackIds.indexOf(laneTrackIds[laneTrackIds.length - 1]) + 1
      : masterTrackIds.indexOf(laneTrackIds[position]);
  if (sourceIndex < 0 || targetIndex < 0) return unchanged();

  const reordered = [...masterTrackIds];
  reordered.splice(sourceIndex, 1);
  const insertionIndex = targetIndex > sourceIndex ? targetIndex - 1 : targetIndex;
  reordered.splice(insertionIndex, 0, sourceTrackId);
  return reordered;
}
