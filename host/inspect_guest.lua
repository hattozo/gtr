-- What the guest's place looks like to a host: lighting and its effects, the local player's tools, and the character
local lines = {}
local function add(text) table.insert(lines, text) end

local lighting = game:GetService("Lighting")
add(("Lighting: ClockTime=%s Brightness=%s Ambient=%s OutdoorAmbient=%s"):format(
    tostring(lighting.ClockTime), tostring(lighting.Brightness), tostring(lighting.Ambient), tostring(lighting.OutdoorAmbient)))
for _, child in ipairs(lighting:GetChildren()) do
    add("  Lighting child: " .. child.ClassName .. " " .. child.Name)
end
for _, child in ipairs(workspace.CurrentCamera:GetChildren()) do
    add("  CurrentCamera child: " .. child.ClassName .. " " .. child.Name)
end

local player = game:GetService("Players").LocalPlayer
add("Player: " .. tostring(player))
if player then
    local backpack = player:FindFirstChildOfClass("Backpack")
    for _, tool in ipairs(backpack and backpack:GetChildren() or {}) do
        add("  Backpack: " .. tool.ClassName .. " " .. tool.Name)
    end
    local character = player.Character
    if character then
        local humanoid = character:FindFirstChildOfClass("Humanoid")
        add(("Character: %s, %d children, Health=%s RigType=%s"):format(character.Name, #character:GetChildren(),
            humanoid and tostring(humanoid.Health) or "?", humanoid and tostring(humanoid.RigType) or "?"))
        local _, size = character:GetBoundingBox()
        add("  bounding box (studs): " .. tostring(size))
    end
end
for _, item in ipairs(game:GetService("StarterPack"):GetChildren()) do
    add("  StarterPack: " .. item.ClassName .. " " .. item.Name)
end
return table.concat(lines, "\n")
