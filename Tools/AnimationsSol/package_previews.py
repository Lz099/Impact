from PIL import Image, ImageDraw, ImageFont
from pathlib import Path

root = Path("D:/UEProjets/Impact/Saved/AnimationsSol")
font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf",18)
title = ImageFont.truetype("C:/Windows/Fonts/arialbd.ttf",22)
names = {"Recroqueville_Au_Sol":"Protection du ventre",
         "Halete_Au_Sol":"Respiration difficile",
         "Tient_Jambe_Au_Sol":"Protection de la jambe droite"}
for name,label in names.items():
    folder = root/name
    frames = []
    for frame in range(0,150,2):
        im = Image.new("RGB",(1120,480),(22,28,35))
        draw = ImageDraw.Draw(im)
        draw.text((14,8),label+" — première version",font=title,fill="white")
        draw.text((14,36),"Boucle 5 s · aperçu 15 i/s · animation source 30 i/s",font=font,fill=(179,194,211))
        for i,view in enumerate(("three_quarter","profile")):
            im.paste(Image.open(folder/"frames"/f"{view}_{frame:03d}.png"),(i*560,60))
        frames.append(im)
    palette = frames[10].quantize(colors=128)
    sequence, durations = [],[]
    for speed,repeat,loops in (("Vitesse normale",1,2),("Ralenti × 0,5",2,1)):
        for cycle in range(loops):
            for i,frame in enumerate(frames):
                im=frame.copy()
                ImageDraw.Draw(im).text((880,15),speed,font=font,fill="white")
                sequence.append(im.quantize(palette=palette))
                durations.append([70,60,70][i%3]*repeat)
    sequence[0].save(folder/"Apercu.gif",save_all=True,append_images=sequence[1:],
                     duration=durations,loop=0,optimize=False)
    sheet=Image.new("RGB",(1440,780),(22,28,35))
    draw=ImageDraw.Draw(sheet)
    draw.text((18,12),label+" — poses à 0 / 1 / 1,5 / 3,5 secondes",font=title,fill="white")
    for row,view in enumerate(("three_quarter","profile")):
        for col,frame in enumerate((0,30,46,106)):
            im=Image.open(folder/"frames"/f"{view}_{frame:03d}.png")
            im.thumbnail((360,330))
            sheet.paste(im,(col*360,60+row*355))
            draw.text((col*360+15,335+row*355),f"{frame/30:.2f} s",font=font,fill="white")
    sheet.save(folder/"Positions.png")
print("PREVIEWS_PACKAGED")
