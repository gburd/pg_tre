import random, sys
n = int(sys.argv[1]); out = sys.argv[2]
random.seed(1234)
stop = "the of and a to in is it for on as at by an be or".split()
roots = ["system","program","network","service","config","module","handler",
 "request","response","session","buffer","kernel","process","thread","socket",
 "packet","router","cache","index","query","schema","cluster","replica","backup",
 "monitor","metric","logger","parser","encoder","decoder","matrix","vector"]
content = [f"{random.choice(roots)}{random.randint(100,9999)}" for _ in range(8000)]
# Planted tokens at controlled frequencies for the cross-engine tests:
#   common ~5%, mid ~1%, rare ~0.1%.
planted = {"government": 0.05, "electrification": 0.01, "naturalize": 0.001}
def typo(w):
    if len(w) < 4: return w
    p = random.randint(1, len(w)-2)
    return w[:p]+random.choice("abcdefghijklmnopqrstuvwxyz")+w[p+1:]
with open(out,"w") as f:
    f.write("id,body\n")
    for i in range(1, n+1):
        k = max(8, int(random.gauss(20, 6)))
        words = []
        for _ in range(k):
            x = random.random()
            if x < 0.5: words.append(random.choice(stop))
            else: words.append(random.choice(content))
        # plant rare tokens probabilistically per row (independent)
        for tok, p in planted.items():
            if random.random() < p:
                words.insert(random.randint(0, len(words)), tok)
        words = [typo(w) if random.random()<0.01 else w for w in words]
        f.write(f'{i},"{" ".join(words)}"\n')
