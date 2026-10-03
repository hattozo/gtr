-- The pistol from "undead coming" (2009), the user's model as it is (guest/models/UndeadPistol.rbxmx, copied into the guest's
-- content), with what time has broken in it mended:
--   - its bullet's script only hurt a Humanoid whose model held a "creator" tag, which nothing's does: it was meant to be the
--     bullet's own tag, so as written it never hurt anyone. It hurts what it hits now, but not the one who fired it;
--   - its bullet's mesh and texture were files of Roblox's 2009 install (rbxasset://../../../shareddata/assets/...): they are
--     the same assets by their ids now;
--   - its bullet left from the hand but flew along the line from the head to the target, so it passed a stud and a half to
--     the side of what was aimed at: wide 2009 characters caught it anyway, but GTA's people's boxes are narrower. It flies
--     from where it leaves through the target now.
-- The rest (the reload message, the old cursors, TargetPoint aiming, the hovering bullet) is left as it was.
local player = game:GetService("Players").LocalPlayer
local MODEL = "rbxasset://gtr/UndeadPistol.rbxmx"
local TAG = "GtrPistol"

local BULLET_SCRIPT = [[
damage = 7
local projectile = script.Parent
game:GetService("Debris"):AddItem(projectile, 8)
local creator = projectile:FindFirstChild("creator")
local shooter = creator and creator.Value and creator.Value.Character

local connection
local function onTouched(hit)
	if hit.Parent == nil or hit.Name == "Projectile" or (shooter and hit:IsDescendantOf(shooter)) then
		return
	end
	local humanoid = hit.Parent:FindFirstChild("Humanoid")
	if humanoid then
		connection:Disconnect()
		humanoid:TakeDamage(damage)
		if creator then
			local tag = creator:Clone()
			tag.Value = creator.Value
			tag.Parent = humanoid
			game:GetService("Debris"):AddItem(tag, 0.25)
		end
	end
	projectile:Destroy()
end
connection = projectile.Touched:Connect(onTouched)
]]

local function makePistol()
    local loaded = game:GetObjects(MODEL)
    local tool = loaded and loaded[1]
    if not tool then
        return nil
    end
    local gun = tool:FindFirstChild("GunScript")
    if gun then
        local aimed, count = gun.Source:gsub('p%.Velocity = %(script%.Parent%.Parent%["Head"%]%.Position %- v%)%.unit %* %-150', "p.Velocity = dir * 150")
        if count == 1 then
            gun.Source = aimed
        end
    end
    local bullet = tool:FindFirstChild("ProjectileScript")
    if bullet then
        bullet.Source = BULLET_SCRIPT
    end
    -- The bullet's mesh, the one straight under the tool (the handle has its own)
    for _, child in ipairs(tool:GetChildren()) do
        if child:IsA("SpecialMesh") then
            child.MeshId = "rbxassetid://2697549"
            child.TextureId = "rbxassetid://2697544"
        end
    end
    local tag = Instance.new("StringValue")
    tag.Name = TAG
    tag.Parent = tool
    return tool
end

local function has(container)
    for _, item in ipairs(container:GetChildren()) do
        if item:IsA("Tool") and item:FindFirstChild(TAG) then
            return true
        end
    end
    return false
end

local starterPack = game:GetService("StarterPack")
if not has(starterPack) then
    local pistol = makePistol()
    if pistol then
        pistol.Parent = starterPack
    end
end
local backpack = player:WaitForChild("Backpack", 15)
if backpack and not has(backpack) and not (player.Character and has(player.Character)) then
    local pistol = makePistol()
    if pistol then
        pistol.Parent = backpack
    end
end
return "pistol ready"
