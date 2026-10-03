-- What the guest adds to whatever place it plays: the character is the classic "noob" (yellow head and arms, blue torso, green
-- legs), and beside the place's own tools the player has the old building tools, Roblox's HopperBins: move, clone, delete, grab.
local player = game:GetService("Players").LocalPlayer
local colors = {
    Head = "Bright yellow", Torso = "Bright blue", ["Left Arm"] = "Bright yellow", ["Right Arm"] = "Bright yellow",
    ["Left Leg"] = "Br. yellowish green", ["Right Leg"] = "Br. yellowish green",
}

local function dress(character)
    for _, child in ipairs(character:GetChildren()) do
        if child:IsA("Shirt") or child:IsA("Pants") or child:IsA("ShirtGraphic") or child:IsA("CharacterMesh") or child:IsA("Accessory") then
            child:Destroy()
        end
    end
    local body = character:FindFirstChildOfClass("BodyColors") or Instance.new("BodyColors")
    body.HeadColor = BrickColor.new(colors.Head)
    body.TorsoColor = BrickColor.new(colors.Torso)
    body.LeftArmColor = BrickColor.new(colors["Left Arm"])
    body.RightArmColor = BrickColor.new(colors["Right Arm"])
    body.LeftLegColor = BrickColor.new(colors["Left Leg"])
    body.RightLegColor = BrickColor.new(colors["Right Leg"])
    body.Parent = character
    for name, color in pairs(colors) do
        local part = character:FindFirstChild(name)
        if part and part:IsA("BasePart") then
            part.BrickColor = BrickColor.new(color)
        end
    end
end

player.CharacterAdded:Connect(function(character)
    task.wait(0.2)
    dress(character)
end)
if player.Character then
    dress(player.Character)
end

local bins = { { "Move", Enum.BinType.GameTool }, { "Clone", Enum.BinType.Clone }, { "Delete", Enum.BinType.Hammer }, { "Grab", Enum.BinType.Grab } }
local function give(pack)
    for _, bin in ipairs(bins) do
        if not pack:FindFirstChild(bin[1]) then
            local hopper = Instance.new("HopperBin")
            hopper.Name = bin[1]
            hopper.BinType = bin[2]
            hopper.Parent = pack
        end
    end
end
give(game:GetService("StarterPack"))
local backpack = player:FindFirstChildOfClass("Backpack")
if backpack then
    give(backpack)
end
