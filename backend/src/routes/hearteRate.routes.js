const { Router } = require('express');
const heartRateController = require('../controllers/HeartRateController');

const router = Router();

router.post('/readings', (req, res) => heartRateController.create(req, res));
router.get('/reports/daily', (req, res) => heartRateController.getReport(req, res));

module.exports = router;