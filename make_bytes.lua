local f, err = io.open("all_bytes.bin", "wb")
if not f then
    error(err)
end

for i = 0, 255 do
    f:write(string.char(i))
end

f:close()
