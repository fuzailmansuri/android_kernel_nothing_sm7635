#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/err.h>
#include <linux/platform_device.h>

#define CLASS_NAME "ois_vdd_ctrl"

struct ois_vdd_ctrl_info {
	struct platform_device *pdev;
	struct class *ois_vdd_class;
	unsigned int ois_vdd_max_uv;
	unsigned int ois_vdd_min_uv;
	struct regulator *ois_vdd;
	bool enable;
};

static struct ois_vdd_ctrl_info *ois_vdd_info;
static DEFINE_MUTEX(ois_vdd_lock);

static int ois_vdd_parse_dt(struct ois_vdd_ctrl_info *ois_vdd_info)
{
	int rc = 0;
	struct device *dev = &ois_vdd_info->pdev->dev;
	struct device_node *np = dev->of_node;

	ois_vdd_info->ois_vdd =  regulator_get(dev, "ois-vdd");
	if (IS_ERR(ois_vdd_info->ois_vdd)) {
		rc = PTR_ERR(ois_vdd_info->ois_vdd);
		dev_err(&ois_vdd_info->pdev->dev,"regulator get failed vdd rc = %d\n",rc);
		goto err_pwr;
	}

	rc = of_property_read_u32(np, "ois-vdd-max-uv", &ois_vdd_info->ois_vdd_max_uv);
	if(rc){
		dev_err(&ois_vdd_info->pdev->dev,"fail to get ois_vdd_max_uv\n");
		goto err_reg;
	}

	rc = of_property_read_u32(np, "ois-vdd-min-uv", &ois_vdd_info->ois_vdd_min_uv);
	if(rc){
		dev_err(&ois_vdd_info->pdev->dev,"fail to get ois_vdd_min_uv\n");
		goto err_reg;
	}

	rc = regulator_set_voltage(ois_vdd_info->ois_vdd, ois_vdd_info->ois_vdd_min_uv, ois_vdd_info->ois_vdd_max_uv);
	if (rc) {
		dev_err(&ois_vdd_info->pdev->dev,"regulator set voltage failed vdd rc = %d\n",rc);
		goto err_reg;
	}

	return rc;

err_reg:
	regulator_put(ois_vdd_info->ois_vdd);
err_pwr:
	return rc;
}

int ois_vdd_enable(struct ois_vdd_ctrl_info *ois_vdd_info)
{
	int rc = regulator_enable(ois_vdd_info->ois_vdd);
	if (rc)
		dev_err(&ois_vdd_info->pdev->dev, "regulator enable failed rc=%d\n", rc);

	return rc;
}

int ois_vdd_disable(struct ois_vdd_ctrl_info *ois_vdd_info)
{
	int rc = 0;

	rc = regulator_disable(ois_vdd_info->ois_vdd);

	return rc;
}

static ssize_t enable_show(struct class *class, struct class_attribute *attr,
                            char *buf)
{
	ssize_t ret;

	mutex_lock(&ois_vdd_lock);
	if (!ois_vdd_info)
		ret = -ENODEV;
	else
		ret = sysfs_emit(buf, "%d\n", ois_vdd_info->enable);
	mutex_unlock(&ois_vdd_lock);
	return ret;
}

static ssize_t enable_store(struct class *class, struct class_attribute *attr,
                            const char *buf, size_t count)
{
	unsigned int val;
	int error;

	error = kstrtouint(buf, 10, &val);
       if (error)
		return error;

	if (val != 0 && val != 1)
		return -EINVAL;

	mutex_lock(&ois_vdd_lock);
	if (!ois_vdd_info) {
		error = -ENODEV;
		goto out;
	}
	if (val == ois_vdd_info->enable) {
		error = 0;
		goto out;
	}

	if (val)
		error = ois_vdd_enable(ois_vdd_info);
	else
		error = ois_vdd_disable(ois_vdd_info);
	if (!error)
		ois_vdd_info->enable = val;
out:
	mutex_unlock(&ois_vdd_lock);
	if (error)
		return error;

	return count;
}

static CLASS_ATTR_RW(enable);

static struct attribute *ois_vdd_class_attrs[] = {
		&class_attr_enable.attr,
		NULL,
};

ATTRIBUTE_GROUPS(ois_vdd_class);

static struct class ois_vdd_class = {
	.name = CLASS_NAME,
	.owner = THIS_MODULE,
	.class_groups = ois_vdd_class_groups,
};

static int ois_vdd_ctrl_probe(struct platform_device *pdev){

	struct ois_vdd_ctrl_info *info;
	int ret;

	mutex_lock(&ois_vdd_lock);
	if (ois_vdd_info) {
		ret = -EBUSY;
		goto out;
	}
	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		ret = -ENOMEM;
		goto out;
	}
	info->pdev = pdev;
	info->ois_vdd_class = &ois_vdd_class;

	ret = ois_vdd_parse_dt(info);
	if (ret) {
		dev_err(&pdev->dev, "ois_vdd_parse_dt failed\n");
		goto err_free;
	}

	/* Class attributes can run as soon as class_register publishes them. */
	ois_vdd_info = info;
	ret = class_register(&ois_vdd_class);
	if (ret) {
		dev_err(&pdev->dev, "class_register fail\n");
		ois_vdd_info = NULL;
		regulator_put(info->ois_vdd);
		goto err_free;
	}

	platform_set_drvdata(pdev, info);
	mutex_unlock(&ois_vdd_lock);
	return 0;
err_free:
	kfree(info);
out:
	mutex_unlock(&ois_vdd_lock);
	return ret;
}

static int ois_vdd_ctrl_remove(struct platform_device *pdev)
{
	struct ois_vdd_ctrl_info *info = platform_get_drvdata(pdev);
	int ret;

	class_unregister(&ois_vdd_class);
	mutex_lock(&ois_vdd_lock);
	ois_vdd_info = NULL;
	platform_set_drvdata(pdev, NULL);
	if (info->enable) {
		ret = ois_vdd_disable(info);
		if (ret)
			dev_err(&pdev->dev, "regulator disable failed rc=%d\n", ret);
	}
	regulator_put(info->ois_vdd);
	kfree(info);
	mutex_unlock(&ois_vdd_lock);
	return 0;
}

static const struct of_device_id ois_vdd_ctrl_of_match[] = {
	{ .compatible = "nt,ois_vdd_ctrl", },
	{ },
};

MODULE_DEVICE_TABLE(of, ois_vdd_ctrl_of_match);

static struct platform_driver ois_vdd_ctrl_driver = {
	.probe		= ois_vdd_ctrl_probe,
	.remove		= ois_vdd_ctrl_remove,
	.driver		= {
		.name	= "ois_vdd_ctrl",
		.of_match_table = ois_vdd_ctrl_of_match,
	}
};
static int __init ois_vdd_ctrl_init(void)
{
	return platform_driver_register(&ois_vdd_ctrl_driver);
}

static void __exit ois_vdd_ctrl_exit(void)
{
	platform_driver_unregister(&ois_vdd_ctrl_driver);
}


module_init(ois_vdd_ctrl_init);
module_exit(ois_vdd_ctrl_exit);
MODULE_DESCRIPTION("nt ois vdd control function");
MODULE_LICENSE("GPL v2");
