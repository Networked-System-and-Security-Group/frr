``` bash
sudo rattan run -c ./midr-pm-test.toml --left-stdout --right-stdout 2>&1 | tee ./rattan.log
```

``` bash
sudo cat bgpd-a.log
```

``` bash
sudo chmod +r ./bgpd-a.log & python3 plot_pm.py ./bgpd-a.log --output ./pm_results.png
```