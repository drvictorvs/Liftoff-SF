ScriptName Liftoff_OSF Hidden

String Function BoolValue(Bool abValue) global
	If abValue
		Return "1"
	EndIf
	Return "0"
EndFunction

String Function ReadSettings() global
	If !OSFSettings.IsReady()
		Return ""
	EndIf

	String settings = BoolValue(OSFSettings.GetBool("liftoff", "enabled", True))
	settings = settings + "|" + OSFSettings.GetString("liftoff", "barColor", "#FFFFFF")
	settings = settings + "|" + OSFSettings.GetInt("liftoff", "showStatus", 1)
	settings = settings + "|" + BoolValue(OSFSettings.GetBool("liftoff", "showPercent", True))
	settings = settings + "|" + BoolValue(OSFSettings.GetBool("liftoff", "useStarfieldFont", True))
	settings = settings + "|" + BoolValue(OSFSettings.GetBool("liftoff", "transparentBackground", False))
	settings = settings + "|" + BoolValue(OSFSettings.GetBool("liftoff", "stayOnTop", True))
	settings = settings + "|" + OSFSettings.GetInt("liftoff", "fadeMs", 500)
	settings = settings + "|" + OSFSettings.GetInt("liftoff", "timeoutSeconds", 240)
	settings = settings + "|" + OSFSettings.GetString("liftoff", "trackedExtensions", ".esm,.esp,.esl,.ba2")
	settings = settings + "|" + BoolValue(OSFSettings.GetBool("liftoff", "logAllFiles", False))
	settings = settings + "|" + BoolValue(OSFSettings.GetBool("liftoff", "logFiles", True))
	settings = settings + "|" + BoolValue(OSFSettings.GetBool("liftoff", "logFailedOpens", False))
	settings = settings + "|" + OSFSettings.GetInt("liftoff", "minOpenMs", 0)
	settings = settings + "|" + OSFSettings.GetInt("liftoff", "maxFileLines", 100000)
	settings = settings + "|" + OSFSettings.GetInt("liftoff", "stallSeconds", 3)
	settings = settings + "|" + OSFSettings.GetInt("liftoff", "sampleSeconds", 5)
	settings = settings + "|" + OSFSettings.GetInt("liftoff", "maxDumps", 60)
	settings = settings + "|" + OSFSettings.GetInt("liftoff", "stackDepth", 32)
	settings = settings + "|" + OSFSettings.GetInt("liftoff", "lookBackSeconds", 10)
	; Added after the first 20; the DLL reads them in this order.
	settings = settings + "|" + OSFSettings.GetString("liftoff", "panelColor", "#5D5D5D")
	settings = settings + "|" + OSFSettings.GetInt("liftoff", "panelOpacity", 100)
	settings = settings + "|" + OSFSettings.GetString("liftoff", "textColor", "#FFFFFF")
	settings = settings + "|" + OSFSettings.GetInt("liftoff", "fontSize", 22)
	settings = settings + "|" + BoolValue(OSFSettings.GetBool("liftoff", "uppercase", True))
	settings = settings + "|" + OSFSettings.GetInt("liftoff", "showOnSaveLoad", 1)
	settings = settings + "|" + OSFSettings.GetString("liftoff", "gameFont", "$MAIN_Font_Bold")
	Return settings
EndFunction
