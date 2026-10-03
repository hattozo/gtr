-- The trainer: a menu, opened with M or its button, that puts Roblox things into the host's world and asks the host for things
-- of its own. The zombies are Roblox characters run from here: they walk at whoever is nearest, the player or one of the host's
-- people, and hit them.
--
-- What the host is to do is asked by leaving a StringValue named HostRequest, holding the message, under workspace.HostCamera:
-- the bridge passes it on to the host's script and takes it away.
local Players = game:GetService("Players")
local UserInputService = game:GetService("UserInputService")
local RunService = game:GetService("RunService")
local player = Players.LocalPlayer

-- Run again (the bridge can be told to): what the last run made goes first
local playerGui = player:WaitForChild("PlayerGui")
local old = playerGui:FindFirstChild("Trainer")
if old then
    old:Destroy()
end
local generation = (_G.GtrTrainerGeneration or 0) + 1
_G.GtrTrainerGeneration = generation

local function askHost(message)
    local camera = workspace:FindFirstChild("HostCamera")
    if camera then
        local request = Instance.new("StringValue")
        request.Name = "HostRequest"
        request.Value = message
        request.Parent = camera
    end
end

local BRICK_COLORS = { "Bright red", "Bright blue", "Bright yellow", "Bright green", "Bright orange", "White", "Medium stone grey" }

local function rootOf(model)
    return model and (model:FindFirstChild("HumanoidRootPart") or model:FindFirstChild("Torso"))
end

------------------------------------------------------------------------------------------------------------------------ zombies
local ZOMBIE_HEALTH = 60
local ZOMBIE_SPEED = 11
local ZOMBIE_DAMAGE = 5
local ZOMBIE_REACH = 4.5
local ZOMBIE_HIT_SECONDS = 1.0
-- Roblox things have ground under them only round the player: a zombie spawns this near, and one that has been left behind or
-- has fallen out of the world is taken away
local SPAWN_DISTANCE = { 14, 24 }
local LOST_DISTANCE = 110
local FALLEN_BELOW = 80
local CORPSE_SECONDS = 4
local MAX_ZOMBIES = 24
local SEES_PEOPLE_WITHIN = 45

local zombies = _G.GtrZombies or {}
_G.GtrZombies = zombies

local function makeZombie(position)
    local source = player.Character
    if not source or #zombies >= MAX_ZOMBIES then
        return nil
    end
    source.Archivable = true
    local model = source:Clone()
    model.Name = "Zombie"
    local unwanted = {}
    for _, item in ipairs(model:GetDescendants()) do
        if item:IsA("LuaSourceContainer") or item:IsA("BackpackItem") or item:IsA("RemoteEvent") or item:IsA("BindableFunction")
            or item:IsA("ForceField") or item.Name == "InputGateway" then
            table.insert(unwanted, item)
        end
    end
    for _, item in ipairs(unwanted) do
        item:Destroy()
    end
    local skin, cloth = BrickColor.new("Bright green"), BrickColor.new("Reddish brown")
    local body = model:FindFirstChildOfClass("BodyColors")
    if body then
        body:Destroy()
    end
    for name, color in pairs({ Head = skin, ["Left Arm"] = skin, ["Right Arm"] = skin, Torso = cloth, ["Left Leg"] = cloth, ["Right Leg"] = cloth }) do
        local part = model:FindFirstChild(name)
        if part then
            part.BrickColor = color
        end
    end
    local humanoid = model:FindFirstChildOfClass("Humanoid")
    local torso = model:FindFirstChild("Torso")
    if not humanoid or not torso then
        model:Destroy()
        return nil
    end
    humanoid.MaxHealth = ZOMBIE_HEALTH
    humanoid.Health = ZOMBIE_HEALTH
    humanoid.WalkSpeed = ZOMBIE_SPEED
    humanoid.DisplayDistanceType = Enum.HumanoidDisplayDistanceType.None

    local zombie = { model = model, humanoid = humanoid, joints = {}, nextHit = 0, phase = math.random() * 6 }
    for _, name in ipairs({ "Left Shoulder", "Right Shoulder", "Left Hip", "Right Hip" }) do
        local joint = torso:FindFirstChild(name)
        if joint then
            zombie.joints[name] = { joint = joint, rest = joint.C0 }
        end
    end
    model:PivotTo(CFrame.new(position))
    model.Parent = workspace
    humanoid.Died:Connect(function()
        zombie.diedAt = os.clock()
    end)
    table.insert(zombies, zombie)
    return zombie
end

local function spawnZombies(count)
    local root = rootOf(player.Character)
    if not root then
        return
    end
    for _ = 1, count do
        local angle = math.random() * math.pi * 2
        local distance = SPAWN_DISTANCE[1] + math.random() * (SPAWN_DISTANCE[2] - SPAWN_DISTANCE[1])
        makeZombie(root.Position + Vector3.new(math.cos(angle) * distance, 4, math.sin(angle) * distance))
    end
end

local function clearZombies()
    for _, zombie in ipairs(zombies) do
        zombie.model:Destroy()
    end
    table.clear(zombies)
end

-- Whoever is nearest a point: the player, or one of the host's people (each a model with a Humanoid, kept by the bridge)
local function nearestVictim(from)
    local best, bestDistance, bestHumanoid = nil, math.huge, nil
    local own = player.Character
    local ownHumanoid = own and own:FindFirstChildOfClass("Humanoid")
    local ownRoot = rootOf(own)
    if ownRoot and ownHumanoid and ownHumanoid.Health > 0 then
        best, bestDistance, bestHumanoid = ownRoot, (ownRoot.Position - from).Magnitude, ownHumanoid
    end
    local bodies = workspace:FindFirstChild("HostBodies")
    if bodies then
        for _, person in ipairs(bodies:GetChildren()) do
            local humanoid = person:IsA("Model") and person:FindFirstChildOfClass("Humanoid")
            local part = humanoid and humanoid.Health > 0 and person:FindFirstChild("Torso")
            if part then
                local distance = (part.Position - from).Magnitude
                if distance < bestDistance and distance < SEES_PEOPLE_WITHIN then
                    best, bestDistance, bestHumanoid = part, distance, humanoid
                end
            end
        end
    end
    return best, bestDistance, bestHumanoid
end

-- Thinking, a few times a second: where to walk, whom to hit, who is gone
task.spawn(function()
    while _G.GtrTrainerGeneration == generation do
        local now = os.clock()
        local playerRoot = rootOf(player.Character)
        for index = #zombies, 1, -1 do
            local zombie = zombies[index]
            local root = rootOf(zombie.model)
            local gone = not root or not zombie.model.Parent
            if not gone and zombie.diedAt then
                gone = now - zombie.diedAt > CORPSE_SECONDS
            elseif not gone and playerRoot then
                gone = (root.Position - playerRoot.Position).Magnitude > LOST_DISTANCE or root.Position.Y < playerRoot.Position.Y - FALLEN_BELOW
            end
            if gone then
                zombie.model:Destroy()
                table.remove(zombies, index)
            elseif not zombie.diedAt then
                local victim, distance, humanoid = nearestVictim(root.Position)
                if victim then
                    zombie.humanoid:MoveTo(victim.Position)
                    if distance < ZOMBIE_REACH and now >= zombie.nextHit then
                        zombie.nextHit = now + ZOMBIE_HIT_SECONDS
                        humanoid:TakeDamage(ZOMBIE_DAMAGE)
                    end
                end
            end
        end
        task.wait(0.2)
    end
end)

-- Moving, each frame: arms held out in front, legs swung as it walks
local heartbeat
heartbeat = RunService.Heartbeat:Connect(function()
    if _G.GtrTrainerGeneration ~= generation then
        heartbeat:Disconnect()
        return
    end
    local now = os.clock()
    for _, zombie in ipairs(zombies) do
        local root = rootOf(zombie.model)
        if root and not zombie.diedAt then
            local walking = root.AssemblyLinearVelocity.Magnitude > 1
            local swing = walking and math.sin(now * 9 + zombie.phase) * 0.7 or 0
            local reach = math.rad(90) + math.sin(now * 3 + zombie.phase) * 0.12
            for name, entry in pairs(zombie.joints) do
                local side = name:find("Right") and 1 or -1
                local angle = name:find("Shoulder") and reach * side or swing * side
                entry.joint.C0 = entry.rest * CFrame.Angles(0, 0, angle)
            end
        end
    end
end)

------------------------------------------------------------------------------------------------------------------------ the menu
local gui = Instance.new("ScreenGui")
gui.Name = "Trainer"
gui.ResetOnSpawn = false

local function label(parent, class, text, position, size)
    local item = Instance.new(class)
    item.Text = text
    item.Position = position
    item.Size = size
    item.BackgroundColor3 = Color3.fromRGB(31, 31, 31)
    item.BackgroundTransparency = 0.25
    item.BorderSizePixel = 0
    item.TextColor3 = Color3.new(1, 1, 1)
    item.Font = Enum.Font.SourceSansBold
    item.TextSize = 18
    item.Parent = parent
    return item
end

local toggle = label(gui, "TextButton", "Trainer (M)", UDim2.new(1, -228, 0, 96), UDim2.new(0, 220, 0, 28))
local panel = Instance.new("Frame")
panel.Position = UDim2.new(1, -228, 0, 130)
panel.BackgroundTransparency = 1
panel.Visible = false
panel.Parent = gui

local rows = 0
local function heading(text)
    local item = label(panel, "TextLabel", text, UDim2.new(0, 0, 0, rows * 30), UDim2.new(0, 220, 0, 26))
    item.BackgroundTransparency = 0.05
    rows += 1
end
local function button(text, action)
    local item = label(panel, "TextButton", text, UDim2.new(0, 0, 0, rows * 30), UDim2.new(0, 220, 0, 26))
    item.Font = Enum.Font.SourceSans
    item.MouseButton1Click:Connect(action)
    rows += 1
end

heading("Roblox")
button("Zombie", function() spawnZombies(1) end)
button("Zombie horde (6)", function() spawnZombies(6) end)
button("Clear zombies", clearZombies)
button("Heal", function()
    local humanoid = player.Character and player.Character:FindFirstChildOfClass("Humanoid")
    if humanoid then
        humanoid.Health = humanoid.MaxHealth
    end
end)
button("Brick in front", function()
    local root = rootOf(player.Character)
    if root then
        local brick = Instance.new("Part")
        brick.Size = Vector3.new(4, 1.2, 2)
        brick.BrickColor = BrickColor.new(BRICK_COLORS[math.random(#BRICK_COLORS)])
        brick.CFrame = root.CFrame * CFrame.new(0, 2, -7)
        brick.Parent = workspace
    end
end)
heading("GTA")
button("Morning", function() askHost('{"t":"host","op":"time","clock":[8,0]}') end)
button("Noon", function() askHost('{"t":"host","op":"time","clock":[12,0]}') end)
button("Evening", function() askHost('{"t":"host","op":"time","clock":[19,0]}') end)
button("Midnight", function() askHost('{"t":"host","op":"time","clock":[0,0]}') end)
button("Clear sky", function() askHost('{"t":"host","op":"weather","name":"EXTRASUNNY"}') end)
button("Rain", function() askHost('{"t":"host","op":"weather","name":"RAIN"}') end)
button("Wanted level +1", function() askHost('{"t":"host","op":"wanted","by":1}') end)
button("Wanted level -1", function() askHost('{"t":"host","op":"wanted","by":-1}') end)
button("Lose the police", function() askHost('{"t":"host","op":"wanted","level":0}') end)
panel.Size = UDim2.new(0, 220, 0, rows * 30)

local function setOpen(open)
    panel.Visible = open
end
toggle.MouseButton1Click:Connect(function()
    setOpen(not panel.Visible)
end)
local keys
keys = UserInputService.InputBegan:Connect(function(input, processed)
    if _G.GtrTrainerGeneration ~= generation then
        keys:Disconnect()
    elseif not processed and input.KeyCode == Enum.KeyCode.M then
        setOpen(not panel.Visible)
    end
end)
gui.Parent = playerGui

-- For a tool that checks the trainer from outside
_G.GtrTrainer = { spawnZombies = spawnZombies, clearZombies = clearZombies, zombies = zombies }
return #zombies
