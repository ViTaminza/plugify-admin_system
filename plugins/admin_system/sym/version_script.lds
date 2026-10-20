{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* hudmenu binding slots (defined by hudmenu_bindings, filled by the plugify module from
           the hudmenu plugin's exported GetClientMenuType/SetClientMenuType) */
        __hudmenu_*;

        /* Exported methods (admin_system.pplugin, group by group). Everything in
           plugins/admin_system/include is generated from the manifest with bin/plugify-gen -
           keep this list in sync. */
        GetPluginVersion;
        GetTranslation;
        GetDatabaseConfig;
        GetMessageType;
        IsCoreLoaded;

        RegisterCategory;
        RegisterItem;

        HasPermission;
        HasFlag;
        IsAdmin;
        GetFlagName;
        GetPermissionsByFlag;
        GetAdminFlags;
        GetAdminPermissions;
        GetAdminImmunity;
        GetAdminExpireTime;
        GetAdminName;
        GetAdminGroupID;
        GetAdminGroupName;
        GetImmunityType;
        AddPlayerAdmin;
        RemovePlayerAdmin;
        AddPlayerLocalFlag;
        RemovePlayerLocalFlag;
        AddPlayerLocalPermission;
        RemovePlayerLocalPermission;
        AddPlayerLocalImmunity;
        RemovePlayerLocalImmunity;

        IsPlayerPunished;
        GetPlayerPunishmentExpired;
        GetPlayerPunishmentReason;
        AddPlayerPunishment;
        AddOfflinePlayerPunishment;
        RemovePlayerPunishment;
        RemoveOfflinePlayerPunishment;

        ShowAdminMenu;
        ShowAdminCategoryMenu;
        ShowAdminLastCategoryMenu;
        ShowAdminItemMenu;

        SendAction;

        OnAdminConnected_Register;
        OnAdminConnected_Unregister;
        OnCoreLoaded_Register;
        OnCoreLoaded_Unregister;
        OnCategoryRegister_Register;
        OnCategoryRegister_Unregister;
        OnAction_Register;
        OnAction_Unregister;
        OnPlayerPunish_Register;
        OnPlayerPunish_Unregister;
        OnOfflinePlayerPunish_Register;
        OnOfflinePlayerPunish_Unregister;
        OnPlayerUnpunish_Register;
        OnPlayerUnpunish_Unregister;
        OnOfflinePlayerUnpunish_Register;
        OnOfflinePlayerUnpunish_Unregister;
        OnPlayerPunishPre_Register;
        OnPlayerPunishPre_Unregister;
        OnOfflinePlayerPunishPre_Register;
        OnOfflinePlayerPunishPre_Unregister;

    local: *;
};
