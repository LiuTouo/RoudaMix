# Studio Pro：音量推桿與 Limiter 的順序

## 範圍與結論

產品按 Fender Studio Pro（原 Studio One Pro）理解。應區分一般混音的 Session／舊 Song 頁面，以及母帶的 Mastering／舊 Project 頁面。

- 一般通道的 inserts 在通道推桿前：官方說明 pre-fader send 不隨通道推桿改變，而且 send 來源始終在 inserts 後。這是由官方路由敘述推得的順序。
- **母帶頁面的 Master Device Rack 明確提供 pre-fader 與 post-fader 兩組 inserts。**
- 不將母帶頁的功能直接當成一般混音頁 Main 通道的規格；本次未核實後者是否有相同的獨立 Post 區域。

## 官方證據

1. [Effects Signal Routing — Configuring Sends](https://fenderstudiopromanual.fender.com/en/Content/Mixing_Topics/Effects_Signal_Routing.htm)
   - 原文："The send source signal is always post-inserts."
   - 同節說明 pre-fader send 的電平不受通道推桿位置影響。
2. [Master Device Rack](https://fenderstudiopromanual.fender.com/en/Content/Mastering_Topics/Master_Device_Rack.htm)
   - 原文："Pre- and post-fader Insert Racks are provided"
   - 同頁將 peak limiting 列為 Master Device Rack 的常見用途，並說明 Master 推桿會影響各種匯出格式的輸出電平。

## 對 Limiter 的意義

以下是由上述處理順序推得的實務說明，不是軟體自動配置保證：

```text
一般通道：音訊 → Inserts → 通道推桿 → 下游
母帶 Master：Pre Inserts → Master 推桿 → Post Inserts → 輸出
```

若 limiter 放在推桿前，後面的正增益仍可提高已限幅訊號的峰值。若要約束經過 Master 推桿後的最終峰值，可將 limiter 放在母帶 Master 的 Post 區域，並避免其後再有增加電平的處理。此時提高 Master 推桿會提高 limiter 的輸入，可能增加限幅量，而不是直接放大已限幅的輸出。

Post 是效果插槽位置，不代表軟體會自動啟用 limiter、搬移插件或保證任意插件設定都能防削波。True-peak 保護仍取決於 limiter 的能力與設定。

## 對 RoudaMix 的設計參考

可保留一般音軌的「效果鏈 → 推桿」，並考慮在輸出軌提供明確的推桿後效果區，讓使用者自行放置 limiter。這是設計建議，尚未實作。

## 驗證界線

只核對官方文件，未實機操作 Studio Pro，未修改 RoudaMix 程式。官方影片無法擷取，故未作為證據；一般混音頁 Main 的獨立 Post 插槽仍未核實。
