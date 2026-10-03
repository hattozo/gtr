-- The RCL, after BenBonez's RCL 2.01: a dark grey brick held out in both hands that fires a laser. Each shot is a ray: what it
-- meets first is hit, and a Humanoid there, the host's people's included, takes the damage. The beam drawn is two thin parts
-- that neither collide nor are found by rays, gone in a few hundredths of a second.
local Players = game:GetService("Players")
local Debris = game:GetService("Debris")
local player = Players.LocalPlayer

local CLIP = 30
local FIRE_SECONDS = 0.13
local RELOAD_SECONDS = 3
local DAMAGE = 10
local SPREAD = 0.15
local RANGE = 999
local FIRE_SOUND = "rbxassetid://13775494"
local RELOAD_SOUND = "rbxassetid://2691591"
local CURSOR = "rbxasset://textures/GunCursor.png"
local CURSOR_RELOADING = "rbxasset://textures/GunWaitCursor.png"
local TAG = "GtrRcl"

local function makeTool()
    local tool = Instance.new("Tool")
    tool.Name = "RCL"
    tool.CanBeDropped = false
    local handle = Instance.new("Part")
    handle.Name = "Handle"
    handle.Size = Vector3.new(1, 1, 2)
    handle.BrickColor = BrickColor.new("Dark stone grey")
    handle.TopSurface = Enum.SurfaceType.Studs
    handle.BottomSurface = Enum.SurfaceType.Inlet
    -- The face that looks away from the one holding it
    handle.FrontSurface = Enum.SurfaceType.Hinge
    handle.Parent = tool
    for name, id in pairs({ Fire = FIRE_SOUND, Reload = RELOAD_SOUND }) do
        local sound = Instance.new("Sound")
        sound.Name = name
        sound.SoundId = id
        sound.Parent = handle
    end
    local tag = Instance.new("StringValue")
    tag.Name = TAG
    tag.Parent = tool
    return tool
end

local function beam(from, to, color)
    local length = (to - from).Magnitude
    local aim = CFrame.new(from, to)
    for _, half in ipairs({ { 0.75, 0.06 }, { 0.25, 0.03 } }) do
        local part = Instance.new("Part")
        part.Name = "_B"
        part.Anchored = true
        part.CanCollide = false
        part.CanQuery = false
        part.CanTouch = false
        part.Locked = true
        part.BrickColor = color
        pcall(function()
            part.Material = Enum.Material.Neon
        end)
        part.Size = Vector3.new(0.2, 0.2, length * 0.5)
        part.CFrame = aim * CFrame.new(0, 0, -length * half[1])
        part.Parent = workspace
        Debris:AddItem(part, half[2])
    end
end

local armed = setmetatable({}, { __mode = "k" })

local function arm(tool)
    if armed[tool] then
        return
    end
    armed[tool] = true
    local handle = tool:WaitForChild("Handle")
    local ammo, reloading, firing, held = CLIP, false, false, false
    local color = player.TeamColor ~= BrickColor.new("White") and player.TeamColor or BrickColor.new("Really red")
    local saved = {}

    local function showAmmo()
        tool.Name = reloading and "[REL]" or "[" .. ammo .. "]"
    end

    local function fire(aim)
        local character = player.Character
        local head = character and character:FindFirstChild("Head")
        if not head then
            return
        end
        local origin = handle.Position
        local spread = SPREAD / 10 * (origin - aim).Magnitude
        local target = aim + Vector3.new((math.random() * 2 - 1) * spread, (math.random() * 2 - 1) * spread, (math.random() * 2 - 1) * spread)
        local start = (head.CFrame * CFrame.new(0.5, 0, 0)).Position
        local params = RaycastParams.new()
        params.FilterType = Enum.RaycastFilterType.Exclude
        params.FilterDescendantsInstances = { character }
        local result = workspace:Raycast(start, (target - start).Unit * RANGE, params)
        local stop = result and result.Position or start + (target - start).Unit * RANGE
        beam(origin, stop, color)
        if result and result.Instance then
            local holder = result.Instance.Parent
            local humanoid = holder and (holder:FindFirstChildOfClass("Humanoid") or (holder.Parent and holder.Parent:FindFirstChildOfClass("Humanoid")))
            if humanoid and humanoid ~= character:FindFirstChildOfClass("Humanoid") then
                if not humanoid:FindFirstChild("creator") then
                    local tag = Instance.new("ObjectValue")
                    tag.Name = "creator"
                    tag.Value = player
                    tag.Parent = humanoid
                    Debris:AddItem(tag, 0.5)
                end
                humanoid:TakeDamage(DAMAGE)
            end
        end
    end

    local function reload(mouse)
        if reloading or ammo == CLIP then
            return
        end
        reloading = true
        handle.Reload:Play()
        mouse.Icon = CURSOR_RELOADING
        showAmmo()
        task.wait(RELOAD_SECONDS)
        ammo = CLIP
        reloading = false
        mouse.Icon = CURSOR
        showAmmo()
    end

    tool.Equipped:Connect(function(mouse)
        mouse.Icon = reloading and CURSOR_RELOADING or CURSOR
        showAmmo()
        mouse.Button1Down:Connect(function()
            held = true
            if firing then
                return
            end
            firing = true
            while held and not reloading and ammo > 0 and tool.Parent == player.Character do
                ammo -= 1
                fire(mouse.Hit.Position)
                handle.Fire:Play()
                showAmmo()
                task.wait(FIRE_SECONDS)
            end
            firing = false
            if ammo <= 0 then
                reload(mouse)
            end
        end)
        mouse.Button1Up:Connect(function()
            held = false
        end)
        mouse.KeyDown:Connect(function(key)
            if key == "r" then
                reload(mouse)
            end
        end)

        -- Both arms out to hold it, as the original does
        task.wait()
        pcall(function()
            local character = tool.Parent
            local torso = character:FindFirstChild("Torso")
            local left, right = character:FindFirstChild("Left Arm"), character:FindFirstChild("Right Arm")
            local leftShoulder, rightShoulder = torso:FindFirstChild("Left Shoulder"), torso:FindFirstChild("Right Shoulder")
            if not (left and right and leftShoulder and rightShoulder) then
                return
            end
            leftShoulder.Part1, rightShoulder.Part1 = nil, nil
            local leftWeld, rightWeld = Instance.new("Weld"), Instance.new("Weld")
            leftWeld.Part0, leftWeld.Part1 = torso, left
            leftWeld.C1 = CFrame.new(0.8, 0.5, 0.4) * CFrame.Angles(math.rad(270), math.rad(40), 0)
            rightWeld.Part0, rightWeld.Part1 = torso, right
            rightWeld.C1 = CFrame.new(-1.2, 0.5, 0.4) * CFrame.Angles(math.rad(270), math.rad(-5), 0)
            leftWeld.Parent, rightWeld.Parent = torso, torso
            saved = { leftShoulder = leftShoulder, rightShoulder = rightShoulder, left = left, right = right, welds = { leftWeld, rightWeld } }
        end)
    end)
    tool.Unequipped:Connect(function()
        held = false
        pcall(function()
            if saved.welds then
                for _, weld in ipairs(saved.welds) do
                    weld:Destroy()
                end
                saved.leftShoulder.Part1, saved.rightShoulder.Part1 = saved.left, saved.right
            end
            saved = {}
        end)
    end)
end

local function look(container)
    for _, item in ipairs(container:GetChildren()) do
        if item:IsA("Tool") and item:FindFirstChild(TAG) then
            arm(item)
        end
    end
    container.ChildAdded:Connect(function(item)
        if item:IsA("Tool") and item:FindFirstChild(TAG) then
            arm(item)
        end
    end)
end

local starterPack = game:GetService("StarterPack")
local alreadyThere = false
for _, item in ipairs(starterPack:GetChildren()) do
    alreadyThere = alreadyThere or (item:IsA("Tool") and item:FindFirstChild(TAG) ~= nil)
end
if not alreadyThere then
    makeTool().Parent = starterPack
end
local backpack = player:WaitForChild("Backpack", 15)
if backpack then
    local has = false
    for _, item in ipairs(backpack:GetChildren()) do
        has = has or (item:IsA("Tool") and item:FindFirstChild(TAG) ~= nil)
    end
    if not has then
        makeTool().Parent = backpack
    end
    look(backpack)
end
player.CharacterAdded:Connect(function(character)
    look(character)
    local pack = player:WaitForChild("Backpack")
    look(pack)
end)
if player.Character then
    look(player.Character)
end
return "RCL ready"
