// Dirty 判定 + 未儲存變更三分支的純邏輯(App.svelte 呼;抽離以便手動驗證)。
// 權威模型:engine 的 revision(所有成功 mutation +1,含 set_param)對上
// client 端記住的 cleanRevision(上次成功 save/load 當下的 revision)。
// reconnect 後 snapshot 帶回 engine 現值 → 自然重對齊。

export type DirtyChoice = "save" | "discard" | "cancel";

/** dirty = 權威 revision 離開了上次存/載的基準(未知 revision = 視為 dirty,保守) */
export function isDirty(revision: number | null, cleanRevision: number | null): boolean {
  if (revision === null) return false; // 還沒連上 engine:無 from 狀態可髒
  if (cleanRevision === null) return false; // 尚無基準(剛連上、未存未載):不擋
  return revision !== cleanRevision;
}

/**
 * 三分支決策:dirty 時才問;cancel 永遠阻止;discard 直接過;
 * save 先存(呼叫端存失敗必須當 cancel 處理 —— 不得覆蓋/退出)。
 */
export function resolveDirtyChoice(
  dirty: boolean,
  choice: DirtyChoice,
): { proceed: boolean; shouldSave: boolean } {
  if (!dirty || choice === "discard") return { proceed: true, shouldSave: false };
  if (choice === "cancel") return { proceed: false, shouldSave: false };
  return { proceed: false, shouldSave: true }; // save:先存,存成功後呼叫端再 proceed
}
