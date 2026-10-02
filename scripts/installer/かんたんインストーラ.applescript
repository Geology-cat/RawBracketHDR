-- RawHDR Composer かんたんインストーラ 1.0.0
--
-- 次の 3 つを行う:
--   1. RawHDR Composer.app を「アプリケーション」フォルダへコピーする
--   2. このアプリだけの検疫情報（com.apple.quarantine）を外し、「開発元を確認できない」で開けない状態を解除する
--      （Gatekeeper そのものは止めない。ほかのアプリには何もしない）
--   3. 初めて起動する
--
-- dmg の中でダブルクリックするとスクリプトエディタが開くので、▶（実行）を押す。

-- property は使わない（property があると、実行後にスクリプトへ値を保存し直そうとし、読み出し専用の
-- ディスクイメージの上では「保存できない」と出るため）。

on run
	set appName to "RawHDR Composer.app"
	set bundleID to "io.github.geology-cat.rawhdrcomposer"
	set installDir to "/Applications"
	-- 検証用: 環境変数 RBH_INSTALLER_TEST にフォルダを指定すると、そこへ入れ、確認の画面を出さず、起動もしない。
	set testDir to system attribute "RBH_INSTALLER_TEST"
	set targetDir to installDir
	if testDir is not "" then set targetDir to testDir
	set srcApp to my findSource(appName)
	if srcApp is "" then return
	set destApp to targetDir & "/" & appName

	if testDir is "" then display dialog "RawHDR Composer 1.0.0 をインストールします。" & return & return & ¬
		"次のことを行います:" & return & ¬
		"1.「アプリケーション」フォルダへコピー" & return & ¬
		"2.「開発元を確認できない」で開けない状態を、このアプリについてだけ解除" & return & ¬
		"3. 初めての起動" & return & return & ¬
		"途中で、管理者のパスワードを求められることがあります。" buttons {"キャンセル", "インストール"} default button "インストール" cancel button "キャンセル" with title "RawHDR Composer かんたんインストーラ" with icon note

	-- 1. コピー（同じ場所から実行したときはコピーしない）
	if srcApp is not destApp then
		if my pathExists(destApp) and testDir is "" then
			display dialog "「アプリケーション」フォルダに RawHDR Composer がすでにあります。新しいものに置き換えますか？" buttons {"キャンセル", "置き換える"} default button "置き換える" cancel button "キャンセル" with title "RawHDR Composer かんたんインストーラ" with icon caution
			my quitIfRunning(bundleID)
		end if
		set copyCmd to "rm -rf " & quoted form of destApp & " && /usr/bin/ditto " & quoted form of srcApp & " " & quoted form of destApp
		try
			do shell script copyCmd
		on error
			-- 書き込めないときは管理者の権限で
			try
				do shell script copyCmd with administrator privileges
			on error errMsg
				display dialog "コピーできませんでした。" & return & errMsg buttons {"OK"} default button "OK" with title "RawHDR Composer かんたんインストーラ" with icon stop
				return
			end try
		end try
	end if

	-- 2. このアプリの検疫情報を外す
	set unquarantineCmd to "/usr/bin/xattr -dr com.apple.quarantine " & quoted form of destApp
	try
		do shell script unquarantineCmd
	on error
		try
			do shell script unquarantineCmd with administrator privileges
		on error errMsg
			display dialog "「開発元を確認できない」の解除ができませんでした。" & return & errMsg & return & return & ¬
				"アプリを開けないときは、使用説明書の「インストールと初めての起動」をご覧ください。" buttons {"OK"} default button "OK" with title "RawHDR Composer かんたんインストーラ" with icon caution
		end try
	end try

	if testDir is not "" then return "ok: " & destApp

	-- 3. 起動
	try
		do shell script "/usr/bin/open " & quoted form of destApp
	on error errMsg
		display dialog "起動できませんでした。" & return & errMsg buttons {"OK"} default button "OK" with title "RawHDR Composer かんたんインストーラ" with icon stop
		return
	end try

	display dialog "インストールが完了し、RawHDR Composer を起動しました。" & return & return & ¬
		"次からは「アプリケーション」フォルダや Launchpad から起動できます。" & return & ¬
		"使い方は、同梱の「使用説明書.pdf」をご覧ください。" buttons {"OK"} default button "OK" with title "RawHDR Composer かんたんインストーラ" with icon note
end run

-- インストールする RawHDR Composer.app を探す（このスクリプトと同じ場所 → マウントしたディスクイメージ → 選んでもらう）。
on findSource(appName)
	try
		set myDir to do shell script "/usr/bin/dirname " & quoted form of (POSIX path of (path to me))
		set candidate to myDir & "/" & appName
		if my pathExists(candidate) then return candidate
	end try
	try
		set found to do shell script "for d in /Volumes/*/; do if [ -d \"$d\"" & quoted form of appName & " ]; then printf '%s' \"$d\"" & quoted form of appName & "; break; fi; done"
		if found is not "" then return found
	end try
	try
		set chosen to choose file with prompt "インストールする RawHDR Composer.app を選んでください" of type {"com.apple.application-bundle"}
		return POSIX path of chosen
	on error
		return ""
	end try
end findSource

on pathExists(p)
	try
		do shell script "/bin/test -e " & quoted form of p
		return true
	on error
		return false
	end try
end pathExists

-- 置き換える前に、起動中なら終了してもらう。
on quitIfRunning(bundleID)
	try
		if application id bundleID is running then
			tell application id bundleID to quit
			delay 1
		end if
	end try
end quitIfRunning
